/*
 * mt_matter.h - C-linkage bridge from the AT+MT command handlers (mt_at.c, C)
 * into the esp_matter / CHIP runtime (implemented in main.cpp, C++).
 *
 * Keeps all C++ (esp_matter/CHIP) out of the C command-handler translation
 * unit: mt_at.c calls these plain-C wrappers, main.cpp implements them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AT+MTSTATE? values. */
enum {
    MT_STATE_UNINIT        = 0,  /* no fabric and no open commissioning window */
    MT_STATE_COMMISSIONING = 1,  /* a commissioning window is open             */
    MT_STATE_OPERATIONAL   = 2,  /* commissioned (>= 1 fabric)                 */
};

/* Current commissioning state (one of MT_STATE_*). */
int mt_matter_state(void);

/* Number of commissioned fabrics. */
int mt_matter_fabric_count(void);

/* Open a basic commissioning window for timeout_s seconds (BLE + DNS-SD).
 * Returns 0 on success, -1 on failure. */
int mt_matter_open_commissioning(int timeout_s);

/* Fill qr/manual with the onboarding QR payload and manual pairing code
 * (null-terminated). Returns 0 on success, -1 on failure. */
int mt_matter_onboarding_codes(char *qr, size_t qr_len, char *manual, size_t manual_len);

/* Erase all Matter data and reboot. */
void mt_matter_factory_reset(void);

/* ---- network transport (C3) -------------------------------------------- */

typedef enum {
    MT_NET_WIFI   = 0,
    MT_NET_THREAD = 1,
} mt_net_transport_t;

/*
 * Report the operational transport, whether it is compiled in and started, and
 * whether it is currently connected. Transport is fixed at build time
 * (ENABLE_MATTER_OVER_THREAD), so this tells the host which image it has.
 * Returns 0 on success.
 */
int mt_matter_net_info(int *transport, int *enabled, int *connected);

/*
 * True when the stored fabric was commissioned on a different transport than
 * this image provides, i.e. the device holds credentials it has no way to use
 * (spec 3.12.1). Backs the optional trailing <mismatch> field on AT+MTNET?.
 *
 * Latched at boot rather than recomputed: the comparison is against a marker
 * that is rewritten the moment the device is commissioned on this transport,
 * so a live read would flip to 0 mid-commissioning and the host would lose the
 * explanation for why the window it is looking at exists.
 */
int mt_matter_transport_mismatch(void);

/* ---- Thread role and mesh identity (0.11.0 task 1, AT+MTTHREAD?) ------- */

/*
 * mt_matter.h, C-visible. Fields carry has_* flags because a detached
 * device reports null for the four ids and the wire renders that as an
 * empty field, never as 0. name is NUL terminated, Thread's own 16-byte
 * limit plus the terminator.
 *
 * role and attached carry no has_* flag: role is a Matter RoutingRoleEnum
 * value CHIP derives from live OpenThread state even when the interface is
 * disabled or detached (design spec 2.1; thread-network-diagnostics-
 * provider.cpp's own not-commissioned null list explicitly excludes
 * RoutingRole), and attached is a plain predicate, not an attribute read.
 */
typedef struct {
    uint8_t  role;            /* Matter RoutingRoleEnum, raw */
    bool     attached;
    bool     has_channel;      uint16_t channel;
    bool     has_panid;        uint16_t panid;
    bool     has_extpanid;     uint64_t extpanid;
    bool     has_partitionid;  uint32_t partitionid;
    char     name[17];
} mt_thread_info_t;

/*
 * Fill *out from the ThreadNetworkDiagnostics cluster (0x0035) on endpoint 0,
 * plus the same attached predicate mt_matter_net_info() above uses. Returns
 * an mt_attr_result_t: MT_ATTR_OK on success, MT_ATTR_ERR_CLUSTER when the
 * cluster is absent (a WiFi image never creates it), which cmd_mtthread
 * (mt_at.c) maps to +MTERR:8 rather than the generic +MTERR:3, since the
 * question here is "does this image speak Thread at all". Never calls into
 * OpenThread directly: every field comes from the same esp_matter provider
 * read path mt_matter_attr_read() uses for any other attribute.
 */
int mt_matter_thread_info(mt_thread_info_t *out);

/* Decode a Matter RoutingRoleEnum value into its AT_MT_SPEC.md 2.1 token.
 * NULL when role is outside the enum's known range (an unmapped SDK value,
 * so an out-of-range role degrades to a decimal number on the wire rather
 * than a lie); cmd_mtthread is the sole caller. */
const char *mt_thread_role_name(uint8_t role);

/* ---- live composition (built at boot from the stored composition) ------ */

/* Number of endpoints this device currently presents, excluding the Root
 * Node on endpoint 0. Zero means unconfigured (design spec section 5.5). */
uint16_t mt_matter_endpoint_count(void);

/*
 * Describe the index'th live endpoint in creation order. Writes its Matter
 * device type ID, assigned endpoint ID, composition variant, and parent
 * index (MT_COMP_NO_PARENT when the endpoint has no parent; otherwise the
 * live-table index, same numbering as mt_composition_t.parent, of its
 * parent). Returns 0 on success, -1 when index is out of range.
 */
int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id, uint8_t *variant,
                            uint8_t *parent_idx);

/* Record an endpoint in the live table as the boot rebuild creates it.
 * parent_idx is the live-table index of this endpoint's parent, or
 * MT_COMP_NO_PARENT when it has none. */
void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant, uint8_t parent_idx);

/*
 * Why an attribute access failed. The AT layer maps these onto +MTERR codes;
 * the mapping lives in mt_at.c so this bridge stays free of the AT error space.
 */
typedef enum {
    MT_ATTR_OK = 0,
    MT_ATTR_ERR_ENDPOINT,   /* no such endpoint                       */
    MT_ATTR_ERR_CLUSTER,    /* no such cluster on that endpoint       */
    MT_ATTR_ERR_ATTRIBUTE,  /* no such attribute in that cluster      */
    MT_ATTR_ERR_TYPE,       /* not an integer-valued attribute        */
    MT_ATTR_ERR_FAILED,     /* runtime failure                        */
    /* C1b/B139 fix round 1: a value out of range for an attribute that
     * IS integer-valued and DOES exist. Distinct from MT_ATTR_ERR_TYPE
     * (which means "this attribute is not the kind AT+MTATTR can carry
     * at all"): this is a bad parameter on a valid attribute, mapped to
     * MT_ERR_BAD_PARAM (+MTERR:1) in mt_at.c, not MT_ERR_ATTR_TYPE
     * (+MTERR:5). mt_attr_result_t had no such code before this; added
     * here rather than overloading MT_ATTR_ERR_TYPE, which would have
     * mapped an out-of-range value to the wrong +MTERR class. */
    MT_ATTR_ERR_VALUE,
    /* DE270/B264 follow-up: the attribute exists, IS integer-valued, and the
     * value offered is in range, but the attribute is served by a cluster
     * Instance (ATTRIBUTE_FLAG_MANAGED_INTERNALLY without
     * ATTRIBUTE_FLAG_WRITABLE, esp_matter_data_model.cpp) and so cannot be
     * written over AT at all. Distinct from both MT_ATTR_ERR_VALUE (the
     * value itself was rejected) and MT_ATTR_ERR_ATTRIBUTE (no such
     * attribute exists here): this attribute exists and the value would be
     * fine, but this path cannot write it. Mapped to MT_ERR_ATTR_READONLY
     * (+MTERR:11) in mt_at.c. */
    MT_ATTR_ERR_READONLY,
} mt_attr_result_t;

/*
 * Read an integer-valued attribute into *out. Returns an mt_attr_result_t.
 *
 * *out carries the value full-width: for a signed attribute it is the value
 * itself, for an unsigned attribute it is the raw 64-bit pattern (a u64
 * above INT64_MAX arrives bit-exact; reinterpret through uint64_t).
 * *is_unsigned (may be NULL) reports which reading applies, taken from the
 * attribute's own type. The flag is also valid when the result is
 * MT_ATTR_ERR_TYPE, so a caller about to WRITE can fetch the signedness
 * first and select its parse: a null nullable reads as MT_ATTR_ERR_TYPE
 * (the AT grammar has no null literal) yet still has a definite signedness
 * and remains writable. On the other error codes the flag is untouched.
 */
int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                        bool *is_unsigned);

/*
 * Write an integer-valued attribute (interpreted per the attribute's current
 * type). Returns an mt_attr_result_t.
 *
 * val is full-width, mirroring the read: for an unsigned attribute it is the
 * raw 64-bit pattern of the unsigned parse. A value outside the attribute's
 * own width returns MT_ATTR_ERR_VALUE (+MTERR:1) instead of truncating; the
 * pre-round-A pipeline cast through 32-bit long and truncated silently.
 *
 * A value that is in width but refused by the attribute's own ember min/max
 * bounds (esp_matter::attribute::update()/set_val()'s ESP_ERR_INVALID_ARG)
 * also returns MT_ATTR_ERR_VALUE (+MTERR:1), fixed in B264: before this it
 * fell through to MT_ATTR_ERR_FAILED, a bare ERROR indistinguishable from a
 * malformed command. A managed-internally (Instance-owned) attribute never
 * reaches that bounds check and keeps its historical MT_ATTR_ERR_FAILED.
 *
 * A write whose value already equals the attribute's current value (a
 * same-value re-push) returns MT_ATTR_OK in both notify modes: the
 * attribute holds exactly what was asked, so the write succeeded by any
 * definition that matters to a host. notify=false used to answer
 * MT_ATTR_ERR_FAILED (a bare ERROR) for this specific case, because
 * esp_matter's set_val_internal() signals a same-value no-op with
 * ESP_ERR_NOT_FINISHED and only the notify=true path (update_or_report())
 * absorbed it into ESP_OK before this fix.
 *
 * notify=true  uses attribute::update(): subscribers and bound devices see the
 *              change, which is what a host-driven change should normally do.
 * notify=false uses attribute::set_val(): the value changes locally without a
 *              report. Used when reflecting a change that came FROM a
 *              controller, so echoing it back does not loop.
 */
int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t val, bool notify);

/*
 * Emit the Switch cluster's InitialPress event (position 1) on ep, i.e. the
 * upstream arduino-esp32 class's click(). Returns an mt_attr_result_t: 0 on
 * success, MT_ATTR_ERR_ENDPOINT/MT_ATTR_ERR_CLUSTER for the usual lookup
 * failures, MT_ATTR_ERR_FAILED if the event send itself fails.
 */
int mt_matter_switch_click(uint16_t ep);

/* ---- temperature level labels (C3) -------------------------------------- */

/*
 * Bounds for AT+MTTEMPLEVELS. Shared between the handler (mt_at.c, which
 * enforces them before calling the bridge below) and the label store the
 * bridge writes into (main.cpp), so the two cannot drift apart.
 */
#define MT_TEMP_LEVEL_MAX_COUNT 16  /* labels per endpoint, 1..this many  */
#define MT_TEMP_LEVEL_MAX_LEN   16  /* bytes per label, excluding the NUL */

/*
 * AT+MTTEMPLEVELS: store the label list backing ep's TemperatureControl
 * SupportedTemperatureLevels attribute (TemperatureLevel-variant cabinets
 * only, composition variant 1; see mt_devtypes.cpp) and mark it dirty so an
 * active subscription refreshes. labels[0..count-1] are NUL-terminated
 * strings; the handler has already checked count and every label against
 * MT_TEMP_LEVEL_MAX_COUNT/MT_TEMP_LEVEL_MAX_LEN, printable ASCII, and no
 * double quote, so this bridge trusts them and only re-checks bounds
 * defensively.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no TemperatureControl cluster,
 * MT_ATTR_ERR_ATTRIBUTE when the cluster is present but is a
 * TemperatureNumber-variant cabinet (no SupportedTemperatureLevels
 * attribute), MT_ATTR_ERR_FAILED for an internal failure.
 *
 * Not persisted, deliberately: the store starts empty every boot and the
 * host is expected to re-send the labels, the same contract as any other
 * attribute state a host writes at startup.
 */
int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count);

/* ---- door lock (C2) ------------------------------------------------------ */

/*
 * AT+MTLOCK: drive the DoorLock cluster's LockState through the 6-arg
 * DoorLockServer::SetLockState() (main.cpp), so the LockOperation event
 * controllers expect is emitted (spec F4); the 2-arg overload writes the
 * attribute silently and is not used here. The firmware never calls this on
 * its own after an allowed AT+MTCMD verdict: actuation timing belongs to the
 * host, this bridge only reports the outcome once the host has acted.
 *
 * <state> is a DlLockState protocol value (0 NotFullyLocked, 1 Locked, 2
 * Unlocked); mt_at.c checks the 0..2 range itself, since those are wire
 * protocol values documented in the AT contract, not SDK enum values that
 * would need reading through an accessor. <source> is an OperationSourceEnum
 * value, checked by mt_at.c against mt_matter_lock_source_max() below.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no DoorLock cluster, MT_ATTR_ERR_FAILED
 * when SetLockState() itself reports failure.
 */
int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source);

/*
 * The two accessors mt_at.c's AT+MTLOCK handler validates <source> against,
 * so this C translation unit never transcribes an SDK enum value: both
 * return values read from OperationSourceEnum in the pinned CHIP header at
 * call time, not literals copied into this file.
 */

/* OperationSourceEnum::kManual: the default <source> when AT+MTLOCK omits it. */
uint8_t mt_matter_lock_source_manual(void);

/*
 * The highest legal OperationSourceEnum value. kUnknownEnumValue, the enum's
 * true upper bound, is explicitly not a valid source (it exists only for
 * decoding an out-of-range wire value); this is the highest value below it.
 */
uint8_t mt_matter_lock_source_max(void);

/* ---- water valve (seven-type batch, task C2) ---------------------------- */

/*
 * The delegate-pool handout pattern later tasks in this batch copy (chime;
 * washer/dishwasher/dryer's OperationalState instances): the endpoint id a
 * pool object serves is not known until esp_matter::endpoint::create()
 * returns it (assigned inside create() itself, per the endpoint-
 * reproducibility rule in the project's CLAUDE.md), but the config struct
 * create() consumes needs the delegate pointer BEFORE that call. So the
 * thunk (mt_devtypes.cpp, which must stay free of CHIP/esp_matter delegate
 * types per this header's own file comment) hands out a slot first with
 * mt_matter_valve_delegate_alloc(), passes it through
 * config.valve_configuration_and_control.delegate, calls create(), then
 * fixes the real endpoint with mt_matter_valve_delegate_set_endpoint() once
 * the id is known. Both accessors trade in an opaque void*: mt_devtypes.cpp
 * never names HearthValveDelegate (main.cpp) or any CHIP type, the same
 * separation mt_air_quality_feature_mask() below keeps for a scalar value.
 *
 * mt_matter_valve_delegate_alloc() returns nullptr once
 * MT_COMP_MAX_ENDPOINTS slots are handed out, so the caller can abort the
 * boot rebuild the same way a failed create() itself would: a failed
 * endpoint create consumes no id, and this firmware aborts the whole
 * composition on any single failure rather than silently skip an entry
 * (see CLAUDE.md, "A failed endpoint::create() consumes no endpoint ID").
 */
void *mt_matter_valve_delegate_alloc(void);
void mt_matter_valve_delegate_set_endpoint(void *delegate, uint16_t ep);

/*
 * AT+MTVALVE: report the host's own actuation as the
 * ValveConfigurationAndControl cluster's CurrentState (and, when level is
 * present, CurrentLevel too), through UpdateCurrentState()/
 * UpdateCurrentLevel() so the ValveStateChanged event a subscribed
 * controller expects is actually emitted (design spec F1). Same split as
 * AT+MTLOCK/mt_matter_lock_state_set() above: the firmware never calls this
 * on its own after an allowed +MTCMD verdict, and doubly so here, since the
 * SDK ignores that verdict's return value regardless (F1: the delegate's
 * Open/Close callbacks cannot fail the command on the wire) and only the
 * host knows when the valve has actually moved.
 *
 * <state> is a ValveStateEnum wire value (0 Closed, 1 Open, 2
 * Transitioning); mt_at.c checks the 0..2 range itself, the same "wire
 * protocol value documented in the AT contract" reasoning AT+MTLOCK's
 * <state> follows. <level> is 0..100, or -1 for "absent": mt_at.c has
 * already validated the range for any value it passes through here.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no ValveConfigurationAndControl cluster,
 * MT_ATTR_ERR_FAILED when the underlying CHIP_ERROR is not CHIP_NO_ERROR.
 */
int mt_matter_valve_state_set(uint16_t ep, uint8_t state, int level);

/* ---- mode select (seven-type batch, task C3) ----------------------------- */

/*
 * mk_mode_select() (mt_devtypes.cpp) points every mode_select endpoint's
 * config.mode_select.delegate at this ONE global manager (design spec F2:
 * SupportedModesManager dispatches on endpoint id internally, so a single
 * instance covers every mode_select endpoint in the composition; there is no
 * per-endpoint object for the SDK to ask for). Returns an opaque void*, the
 * same shape mt_matter_valve_delegate_alloc() uses above, so mt_devtypes.cpp
 * never has to name HearthSupportedModesManager or any CHIP delegate type.
 */
void *mt_matter_mode_select_manager(void);

/*
 * Bounds for AT+MTMODES. Shared between the handler (mt_at.c, which enforces
 * them before calling the bridge below) and the per-endpoint store the
 * bridge writes into (main.cpp), so the two cannot drift apart.
 */
#define MT_MODES_MAX_COUNT     8   /* mode/label pairs per endpoint, 1..this many */
#define MT_MODES_MAX_LABEL_LEN 32  /* bytes per label, excluding the NUL          */

/*
 * AT+MTMODES: replace ep's ModeSelect SupportedModes list (host-fed, never
 * persisted; the host re-sends it every boot, the same contract
 * AT+MTTEMPLEVELS follows). modes[0..count-1]/labels[0..count-1] are
 * parallel arrays; the handler has already checked count, mode-value
 * uniqueness within the list, and every label against
 * MT_MODES_MAX_COUNT/MT_MODES_MAX_LABEL_LEN, printable ASCII, and no double
 * quote, so this bridge trusts them and only re-checks bounds defensively.
 *
 * Marks SupportedModes dirty (MatterReportingAttributeChangeCallback, the
 * same call AT+MTTEMPLEVELS uses) so an active subscription sees the new
 * list. CurrentMode is a plain, esp_matter-managed attribute (created by
 * mode_select::create() from config.current_mode, esp_matter_cluster.cpp):
 * it is readable and writable over AT+MTATTR like any other integer
 * attribute, and it is NOT reported here. A controller's ChangeToMode
 * command sets CurrentMode itself after validating membership against this
 * same manager (design spec F2, mode-select-server.cpp's ChangeToMode()); the
 * host observes that the ordinary way, a +MTATTR URC from the attribute
 * callback in main.cpp, not through this bridge.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no ModeSelect cluster, MT_ATTR_ERR_FAILED
 * for an internal failure (store exhaustion; cannot happen in practice, one
 * slot per MT_COMP_MAX_ENDPOINTS, same reasoning as the temp-levels store).
 */
int mt_matter_modes_set(uint16_t ep, const uint8_t *modes, const char *const *labels, uint8_t count);

/* ---- ModeBase: RVC run/clean mode, microwave mode (RVC + Microwave batch, task 2) --- */

/*
 * Bounds for AT+MTMODES's cluster-aware form (AT_MT_SPEC.md 3.20). Shared
 * between the handler (mt_at.c, which enforces them before calling the
 * bridge below) and the per-(endpoint, cluster) store the bridge writes into
 * (main.cpp), so the two cannot drift apart. MT_MB_MAX_COUNT/
 * MT_MB_MAX_LABEL_LEN mirror MT_MODES_MAX_COUNT/MT_MODES_MAX_LABEL_LEN
 * above; MT_MB_MAX_LISTS is the number of (endpoint, cluster) lists this
 * firmware can store, a distinct axis from MT_COMP_MAX_ENDPOINTS because a
 * single RVC endpoint carries TWO ModeBase clusters at once (RvcRunMode and
 * RvcCleanMode, design spec section 2.1), so the store is keyed by the pair,
 * not by endpoint alone.
 *
 * Raised 8 -> 12 for energy round C1 (design spec 2.1): the round's Phase 3
 * composition adds two ModeBase slots (battery storage's DEM Mode and the
 * standalone DEM device's DEM Mode) on top of the eight the RVC/appliance
 * rounds consume. Round C2's EVSE draws two more of its own (EnergyEvseMode
 * plus its DEM Mode), which would have landed the pool at exactly zero
 * headroom. B247 established that exact fit is a defect waiting to happen
 * and C1 deliberately ended the exact-fit era, so this raises to 16 instead
 * of leaving a future round to discover it the hard way. Measured cost
 * (build_b4, WiFi image, this raise in isolation): 12 -> 16 cost 1264 bytes
 * of .bss (316 bytes/slot) and 64 bytes of .data, diffed before and after
 * the constant change with nothing else touched.
 */
#define MT_MB_MAX_LISTS     16  /* (endpoint, cluster) lists this firmware can store */
#define MT_MB_MAX_COUNT     8   /* mode/tag/label triples per list, 1..this many     */
#define MT_MB_MAX_LABEL_LEN 32  /* bytes per label, excluding the NUL                */

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()'s
 * doc comment above, which this mirrors), with one difference: ModeBase
 * needs a delegate per (endpoint, cluster) PAIR, not per endpoint alone,
 * because RvcRunMode and RvcCleanMode can both live on the same RVC
 * endpoint and each needs its own delegate object (F7's one-delegate-per-
 * Instance rule: ModeBase::Delegate::SetInstance() VerifyOrDies if a second
 * Instance tries to share one, the identical shape the OperationalState pool
 * above is built around). The cluster id is fixed at ALLOC time here, not
 * after: unlike the endpoint id, it is already known to the caller
 * (mt_devtypes.cpp, Tasks 3-4 of this batch) before create() runs - it is
 * literally which cluster::create() the thunk is about to call - and the
 * delegate needs it immediately, since ModeBase::Instance::Init() reads
 * GetModeValueByIndex(0, ...) before set_endpoint() has any chance to run
 * (see the placeholder-mode reasoning on mt_matter_modebase_set() below).
 *
 * mt_matter_modebase_delegate_alloc() returns nullptr once MT_MB_MAX_LISTS
 * slots are handed out, so the caller aborts the boot rebuild the same way a
 * failed create() itself would (see mt_matter_valve_delegate_alloc()'s
 * comment above for the full reasoning).
 */
void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id);
void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep);

/*
 * AT+MTMODES's cluster-aware form: replace the (ep, cluster) ModeBase
 * cluster's SupportedModes list, host-fed, never persisted, the same
 * re-sent-every-boot contract as the ModeSelect form
 * (mt_matter_modes_set() above) and every other host-fed list in this
 * header. cluster must be one of the ModeBase cluster ids (RvcRunMode,
 * RvcCleanMode, MicrowaveOvenMode, and - composed appliance round -
 * RefrigeratorAndTemperatureControlledCabinetMode from task 3 and OvenMode
 * from task 4); this bridge is the sole
 * place that validates it, since mt_at.c stays free of any esp_matter/CHIP
 * header and cannot read those ids itself (see this file's own top
 * comment). modes[0..count-1]/tags[0..count-1]/labels[0..count-1] are
 * parallel arrays; the handler has already checked count, mode-value
 * uniqueness, tag range (<= 0xFFFF) and every label against
 * MT_MB_MAX_COUNT/MT_MB_MAX_LABEL_LEN, printable ASCII and no double quote,
 * so this bridge trusts them and only re-checks bounds defensively.
 *
 * tags[i] == 0 is substituted at STORE time (not read time) with the
 * cluster's conformance-required default, design spec section 9's exact
 * policy: RvcRunMode's first declared mode gets kIdle, every later one
 * kCleaning; RvcCleanMode gets kVacuum on every mode; MicrowaveOvenMode gets
 * kNormal on every mode; RefrigeratorAndTemperatureControlledCabinetMode gets
 * kAuto (0x00, the ModeBase common tag every mode gets, Mode_Refrigerator.xml)
 * on every mode; OvenMode gets kBake (0x4000, Mode_Oven.xml's first
 * oven-specific tag) on every mode. Substituting at store time rather than
 * in the delegate's
 * GetModeTagsByIndex() keeps every read branch-free. A nonzero tag passes
 * through unvalidated beyond the u16 range check mt_at.c already performed:
 * tag semantics beyond that are the host's business.
 *
 * Also clamps CurrentMode when the replacement list drops the value it
 * currently holds (main.cpp), through the SDK's own
 * ModeBase::Instance::UpdateCurrentMode(), so a controller never observes a
 * CurrentMode that is not a member of the just-published SupportedModes.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when cluster is not one of the ModeBase ids above, or
 * ep has no such cluster, MT_ATTR_ERR_FAILED for a bad count or an internal
 * failure (store exhaustion: MT_MB_MAX_LISTS is smaller than
 * MT_COMP_MAX_ENDPOINTS (mt_composition.h) - cited by name rather than
 * value here, since both have already moved once (8 -> 12 -> 16 across
 * energy rounds C1/C2, 24 -> 28 in the same span) and a number copied into
 * this sentence goes stale exactly when either constant next changes - so a
 * composition with enough ModeBase cluster instances CAN exhaust the store
 * before running out of endpoints; refused with this code rather than
 * silently overwriting an unrelated (endpoint, cluster) slot).
 */
int mt_matter_modebase_set(uint16_t ep, uint32_t cluster, const uint8_t *modes, const uint16_t *tags,
                            const char *const *labels, uint8_t count);

/* ---- OperationalState trio (seven-type batch, task C4) ------------------ */

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()
 * above, which this mirrors exactly): F7 requires one delegate OBJECT per
 * endpoint (esp-matter's OperationalState::Delegate::SetInstance()
 * VerifyOrDies if a second Instance tries to share one), so dish_washer,
 * laundry_washer and laundry_dryer each hand out their own object from this
 * ONE shared pool rather than pointing at a single global the way mode
 * select does. The real endpoint id is not known until create() returns it,
 * same reasoning as the valve pool, so the thunk (mt_devtypes.cpp) allocates
 * a slot first, passes it through config.operational_state.delegate, calls
 * create(), then fixes the real endpoint with
 * mt_matter_opstate_delegate_set_endpoint() once it is known. Opaque void*,
 * so mt_devtypes.cpp never has to name HearthOpStateDelegate or any CHIP
 * delegate type.
 *
 * cluster_id (composed appliance round, task 4) is fixed at ALLOC time, the
 * same reasoning mt_matter_modebase_delegate_alloc() above documents: the
 * caller already knows which cluster it is about to create, the delegate
 * needs it before any endpoint id exists, and one delegate CLASS now serves
 * two clusters. The base OperationalState cluster (0x0060, the washer trio
 * and the microwave) passes OperationalState::Id; the oven cavity's
 * OvenCavityOperationalState (0x0048) passes its own id, which is what makes
 * that endpoint's Stop/Start forwards carry cluster 0x0048 rather than
 * 0x0060, and what routes Pause/Resume (disallowConform in
 * OperationalState_Oven.xml) to the unsupported answer instead of a forward.
 * RvcOperationalState is NOT in this pool: it needs its own Delegate
 * subclass for GoHome, so it keeps the separate pool below.
 *
 * mt_matter_opstate_delegate_alloc() returns nullptr once
 * MT_COMP_MAX_ENDPOINTS slots are handed out, so the caller aborts the boot
 * rebuild the same way a failed create() itself would (see
 * mt_matter_valve_delegate_alloc()'s doc comment above for the full
 * reasoning).
 */
void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id);
void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep);

/*
 * AT+MTOPSTATE: set ep's OperationalState-family cluster CurrentState
 * through the SDK's own Instance::SetOperationalState(), so the
 * OperationalState attribute report a subscribed controller expects is
 * actually emitted (main.cpp's HearthOpStateDelegate/HearthRvcOpStateDelegate
 * class comments have the full F7/task-3 citation trails, including why
 * this needs no registry beyond the delegate pools themselves). Same
 * split-ownership shape as AT+MTLOCK/AT+MTVALVE: the firmware never calls
 * this on its own after an allowed +MTCMD verdict, since only the host
 * knows when the physical appliance has actually completed the transition.
 *
 * ep's cluster decides which state space is legal, checked here (not just
 * in mt_at.c) so a plain OperationalState endpoint can never be handed an
 * RVC-only derived-cluster state:
 *   - ep has the base OperationalState cluster (0x0060, dish/laundry
 *     washer/dryer): <state> is 0 Stopped, 1 Running, 2 Paused only.
 *   - ep has RvcOperationalState (0x0061, RVC + Microwave batch task 3):
 *     <state> is one of {0, 1, 2, 0x40 kSeekingCharger, 0x41 kCharging,
 *     0x42 kDocked}.
 *   - ep has OvenCavityOperationalState (0x0048, composed appliance round
 *     task 4, the heater cabinet under an Oven): <state> is 0, 1, 2 only.
 *     The cluster derives from OperationalState and defines NO derived
 *     number-space states of its own (OperationalState_Oven.xml adds only
 *     command conformance, no state enum), so its legal set is the plain
 *     one, identical to the base cluster's.
 * mt_at.c's cmd_mtopstate rejects anything outside the UNION of all three
 * sets with +MTERR:1 before this is ever called (it cannot know which cluster
 * ep actually has, and the union is unchanged by task 4: the cavity's set is
 * a subset of the base cluster's, so the handler needs no edit at all); this
 * bridge is what narrows that down to the cluster-specific legal set,
 * answering MT_ATTR_ERR_VALUE (+MTERR:1) for a state that is in the union but
 * not legal for ep's own cluster - e.g. 0x40 on a plain washer or an oven
 * cavity endpoint. 3 (Error) is outside the union entirely and is
 * rejected by mt_at.c itself for every cluster kind: kError is reserved for
 * the error-detection path (F7), never a state this command may set
 * directly; SetOperationalState() enforces the identical rule independently
 * (see main.cpp), so a state that somehow slipped past both checks would
 * still be refused there, not silently accepted.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no OperationalState-family cluster at all,
 * MT_ATTR_ERR_VALUE for a state outside the cluster-specific legal set
 * above, MT_ATTR_ERR_FAILED when no delegate/Instance is reachable for ep
 * (should not happen once esp_matter::start() has run, see main.cpp) or
 * SetOperationalState() itself reports failure.
 */
int mt_matter_opstate_set(uint16_t ep, uint8_t state);

/* ---- RVC OperationalState (RVC + Microwave batch, task 3) --------------- */

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()
 * above, which this mirrors): one delegate OBJECT per endpoint, same F7
 * VerifyOrDie reasoning as mt_matter_opstate_delegate_alloc() above, sized
 * by MT_COMP_MAX_ENDPOINTS for the same reason (one RvcOperationalState
 * cluster per RVC endpoint, unlike ModeBase's two-per-endpoint shape).
 *
 * Unlike every other cluster::create() in this codebase,
 * rvc_operational_state::create() wires no delegate and calls no
 * command::create_* on its own (its config_t is an empty struct with no
 * delegate field at all: this cluster is fully app-owned, design spec
 * section 9). mt_devtypes.cpp's mk_rvc()/mt_rvc_opstate_add_commands()
 * therefore both allocate this delegate before create() runs (matching
 * every other pool's before/after shape) AND hand it to
 * esp_matter::cluster::set_delegate_and_init_callback() themselves, since no SDK
 * init callback exists to do it automatically the way
 * OperationalStateDelegateInitCB does for the base OperationalState
 * cluster.
 *
 * mt_matter_rvc_opstate_delegate_alloc() returns nullptr once
 * MT_COMP_MAX_ENDPOINTS slots are handed out, same abort-the-boot-rebuild
 * contract as every other pool in this file.
 */
void *mt_matter_rvc_opstate_delegate_alloc(void);
void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep);

/* ---- air quality (C1b, bug B139) ------------------------------------ */

/*
 * The four optional AirQuality features (Fair, Moderate, VeryPoor,
 * ExtremelyPoor) have to be identical on both sides of the C1b bridge: the
 * ember cluster's feature map (mt_devtypes.cpp's mk_air_quality_sensor(),
 * esp-matter's per-feature cluster::air_quality::feature::*::add() calls)
 * and the CHIP server Instance's BitMask<Feature> (main.cpp's
 * mt_air_quality_register_all()). Fix round 1: these used to be two
 * independently-hardcoded lists (four esp-matter namespace calls in one
 * file, four CHIP Feature enum values in the other) with nothing stopping
 * them drifting apart if a feature were ever added or removed on one side
 * only. This accessor is now the single source of truth for the enabled
 * set, same precedent as mt_matter_lock_source_max() above: read the SDK's
 * own bit values through a function at call time, don't transcribe them
 * into two places. Bit assignment matches the CHIP Feature enum exactly
 * (AirQuality/Enums.h: kFair=0x1, kModerate=0x2, kVeryPoor=0x4,
 * kExtremelyPoor=0x8), which is also what mt_devtypes.cpp tests each bit
 * against since it has no reason to pull in the CHIP Feature enum type
 * itself for four plain add() calls.
 */
uint32_t mt_air_quality_feature_mask(void);

/* ---- smoke/co alarm + refrigerator alarm (seven-type batch task C5;
 * cluster-aware dispatch added composed appliance round task 3) ----------- */

/*
 * AT+MTALARM: set field <field> of ep's alarm cluster, dispatched by which
 * cluster <ep> actually carries (this bridge is the sole place that checks
 * that; mt_at.c stays free of any esp_matter/CHIP header). mt_at.c's
 * cmd_mtalarm only checks the UNION bound, field <= 11, before this is ever
 * called: SmokeCoAlarm's own legal range is 1..11 (field 0, ExpressedState,
 * is derived and never settable, rejected here rather than in the handler,
 * since field 0 is a legal DoorOpen bit for the other cluster); RefrigeratorAlarm's
 * own legal range is 0..7 (an alarm bit number).
 *
 * SmokeCoAlarm branch (unchanged behaviour from before this task, field 0's
 * rejection aside): sets one of the cluster's eleven event-emitting Set*
 * methods (SmokeCoAlarmServer::Instance(), design spec F4), never a raw
 * AT+MTATTR write: the setters fire the cluster's spec-mandated events
 * (SmokeAlarm, COAlarm, MuteEnded, HardwareFault, EndOfService, AllClear,
 * LowBattery, SelfTestComplete) and the critical-alarm auto-unmute, both of
 * which a raw write to the same ember-managed attribute storage would
 * silently skip. This bridge maps field to the matching setter and
 * range-checks <value> against THAT field's own SDK enum bound (each setter
 * takes a differently-typed enum, so mt_at.c, plain C with no SDK access,
 * cannot do this itself); the two boolean fields (TestInProgress,
 * HardwareFaultAlert) are checked against 0/1 instead. Field 5
 * (TestInProgress) value 0 is the self-test completion path: the SDK's own
 * SetTestInProgress() recognises the true->false edge and fires
 * SelfTestComplete, the far end of the notify-only +MTCMD:0,... a
 * controller's SelfTestRequest raises (mt_cmd_notify(), C1; see
 * emberAfPluginSmokeCoAlarmSelfTestRequestCommand() in main.cpp).
 *
 * RefrigeratorAlarm branch (composed appliance round, task 3): <field> is
 * the alarm BIT number (0 DoorOpen, the only bit the Matter spec defines in
 * any revision through 1.5.1), <value> is 0/1. Checked against the
 * endpoint's own Supported bitmap (RefrigeratorAlarmServer::
 * Instance().GetSupportedValue()) before being written through SetStateValue,
 * which writes the State attribute AND emits the cluster's Notify event
 * itself (refrigerator-alarm-server.cpp:115-149) - the same "one call, both
 * effects" reason AT+MTATTR is not used here either.
 *
 * An out-of-range <value> or an unsupported bit returns MT_ATTR_ERR_VALUE
 * (+MTERR:1, the C1b/B139 precedent); a setter returning false (the field
 * already held that value, or the underlying ember write failed) maps to
 * MT_ATTR_ERR_FAILED, a bare ERROR.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has neither alarm cluster, MT_ATTR_ERR_VALUE
 * for a <field>/<value> outside the matched cluster's own range,
 * MT_ATTR_ERR_FAILED when the setter itself reports failure.
 */
int mt_matter_alarm_set(uint16_t ep, uint8_t field, uint8_t value);

/* ---- chime (seven-type batch, task C6) ------------------------------------ */

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()'s
 * doc comment above, which this mirrors exactly): design spec F3, chime's own
 * trap is a null-pointer check rather than a VALIDATE_FEATURES macro
 * (esp_matter_cluster.cpp's cluster::chime::create() returns NULL outright
 * when config.delegate is null), but the shape is identical to the valve/
 * OperationalState pools: the real endpoint id is not known until
 * esp_matter::endpoint::create() returns it, so the thunk (mt_devtypes.cpp,
 * which stays free of CHIP/esp_matter delegate types per this header's file
 * comment) hands out a slot first with mt_matter_chime_delegate_alloc(),
 * passes it through config.chime.delegate, calls create(), then fixes the
 * real endpoint with mt_matter_chime_delegate_set_endpoint() once it is
 * known. Opaque void*, the same shape as every other pool pair in this
 * header. mt_matter_chime_delegate_alloc() returns nullptr once
 * MT_COMP_MAX_ENDPOINTS slots are handed out, so the caller aborts the boot
 * rebuild the same way a failed create() itself would.
 */
void *mt_matter_chime_delegate_alloc(void);
void mt_matter_chime_delegate_set_endpoint(void *delegate, uint16_t ep);

/*
 * Bounds for AT+MTCHIMESOUNDS. Shared between the handler (mt_at.c, which
 * enforces them before calling the bridge below) and the per-endpoint store
 * the bridge writes into (main.cpp), so the two cannot drift apart. The name
 * bound is well under the cluster's own kMaxChimeSoundNameSize (48,
 * ChimeCluster.h), the same "firmware bound tighter than the SDK cap, line
 * stays bounded" reasoning MT_MODES_MAX_LABEL_LEN follows for ModeSelect.
 */
#define MT_CHIME_MAX_SOUNDS   8   /* id/name pairs per endpoint, 1..this many */
#define MT_CHIME_MAX_NAME_LEN 32  /* bytes per name, excluding the NUL       */

/*
 * AT+MTCHIMESOUNDS: replace ep's Chime InstalledChimeSounds list (host-fed,
 * never persisted; the host re-sends it every boot, the same contract
 * AT+MTMODES/AT+MTTEMPLEVELS follow). ids[0..count-1]/names[0..count-1] are
 * parallel arrays; the handler has already checked count, id uniqueness
 * within the list, and every name against MT_CHIME_MAX_SOUNDS/
 * MT_CHIME_MAX_NAME_LEN, printable ASCII, and no double quote, so this
 * bridge trusts them and only re-checks bounds defensively.
 *
 * Feeds the per-endpoint store HearthChimeDelegate::GetChimeSoundByIndex()/
 * GetChimeIDByIndex() (main.cpp) read from, and marks InstalledChimeSounds
 * dirty so an active subscription sees the new list; see the bridge's own
 * comment for why that uses MatterReportingAttributeChangeCallback() rather
 * than the SDK's documented (but, on this revision, nonexistent)
 * ReportInstalledChimeSoundsChange().
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no Chime cluster, MT_ATTR_ERR_FAILED for a
 * bad count or an internal failure (store exhaustion; cannot happen in
 * practice, one slot per MT_COMP_MAX_ENDPOINTS, same reasoning as the
 * temp-levels/modes stores).
 */
int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t *ids, const char *const *names, uint8_t count);

/*
 * AT+MTCHIME: set one of the Chime cluster's two plain attributes on ep
 * through the SDK's own ChimeCluster::SetSelectedChime()/SetEnabled()
 * (design spec F3), not a raw attribute write: this cluster is registered
 * directly with esp_matter's data model provider (see mt_matter.h's
 * mt_matter_chime_delegate_alloc() comment and the F6 workaround this file's
 * registration-window function documents in main.cpp), not through
 * esp_matter's generic attribute store, so there is no esp_matter::
 * attribute::update() path here the way there is for e.g. AT+MTATTR.
 *
 * <what>: 0 SelectedChime (value is a chimeID; SetSelectedChime() answers
 * Status::NotFound, mapped to MT_ATTR_ERR_VALUE, when it is not one of the
 * ids AT+MTCHIMESOUNDS installed), 1 Enabled (value is 0/1, mt_at.c has
 * already checked the range). Anything else in <what> is rejected in mt_at.c
 * with +MTERR:1 before this is ever called.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no Chime cluster, MT_ATTR_ERR_VALUE for an
 * unsupported chimeID, MT_ATTR_ERR_FAILED when the cluster is not reachable
 * through the data model provider's registry (should not happen once
 * esp_matter::start() has run, see main.cpp).
 */
int mt_matter_chime_set(uint16_t ep, uint8_t what, uint8_t value);

/* ---- Microwave Oven Control (RVC + Microwave batch, task 4) ------------- */

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()'s
 * doc comment above, which this mirrors): one delegate OBJECT per endpoint,
 * the same F7 VerifyOrDie reasoning as mt_matter_opstate_delegate_alloc()
 * above, sized by MT_COMP_MAX_ENDPOINTS for the same reason (one
 * MicrowaveOvenControl cluster per microwave endpoint).
 *
 * Unlike every other pool in this file, this cluster's own SDK init callback
 * (MicrowaveOvenControlDelegateInitCB, esp_matter_delegate_callbacks.cpp)
 * looks up TWO other delegates by cluster id (MicrowaveOvenMode,
 * OperationalState, both fetched through cluster::get()+get_delegate_impl())
 * and VerifyOrReturns - silently creating no Instance at all, no log, no
 * error - if either comes back null alongside this one. So the microwave
 * thunk (mt_devtypes.cpp) must allocate and wire all three delegate pools
 * before microwave_oven::create() runs: this one (config.microwave_oven_
 * control.delegate), the ModeBase pool keyed by MicrowaveOvenMode::Id
 * (config.microwave_oven_mode.delegate, mt_matter_modebase_delegate_alloc()
 * above), and the plain OperationalState pool
 * (config.operational_state.delegate, mt_matter_opstate_delegate_alloc()
 * above) - the same three-non-null precondition the class comment on
 * HearthMwocDelegate (main.cpp) documents in full.
 *
 * mt_matter_mwoc_delegate_alloc() returns nullptr once MT_COMP_MAX_ENDPOINTS
 * slots are handed out, same abort-the-boot-rebuild contract as every other
 * pool in this file.
 */
void *mt_matter_mwoc_delegate_alloc(void);
void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep);

/* ---- electrical measurement (energy round A, task 2) -------------------- */

/*
 * How many measurement-capable endpoints one composition can carry. A
 * deliberate step down from MT_COMP_MAX_ENDPOINTS (28): each slot pairs an
 * ElectricalPowerMeasurement delegate (seven Nullable<int64_t> values plus
 * vtable) with a PowerTopology delegate, the same sizing reasoning
 * MT_MB_MAX_LISTS documents above.
 *
 * Raised 4 -> 8 for energy round C1 (design spec 2.1): Phase 3's four EPM/PT
 * pairs from the electrical rounds consumed the pool exactly, and this round
 * adds two more (solar, battery storage), with one for C2's EVSE and one
 * slot of headroom. Unlike Round B's exact fit, the headroom is deliberate:
 * the pools are proven and the exact-fit experiment is concluded.
 */
#define MT_MEAS_MAX 8  /* measurement-capable endpoints per composition */

/*
 * AT+MTMEAS field ids, the wire contract task 3 documents in AT_MT_SPEC.md
 * and task 6 consumes in the host library. MT_MEAS_F_* select
 * ElectricalPowerMeasurement attributes (cluster 0x0090), MT_ENERGY_F_*
 * select ElectricalEnergyMeasurement cumulative counters (cluster 0x0091);
 * which family applies is decided by the cluster id the host passed, so the
 * two spaces may overlap numerically. Units follow the cluster XML exactly
 * (voltage_mv / amperage_ma / power_mw; Frequency is mHz 0..1000000;
 * PowerFactor is 1/100 of a percent, -10000..10000; energy is mWh,
 * 0..2^62).
 */
#define MT_MEAS_F_VOLTAGE        0  /* mV,  signed */
#define MT_MEAS_F_ACTIVE_CURRENT 1  /* mA,  signed */
#define MT_MEAS_F_ACTIVE_POWER   2  /* mW,  signed */
#define MT_MEAS_F_FREQUENCY      3  /* mHz, signed */
#define MT_MEAS_F_POWER_FACTOR   4  /* 1/100 %, signed */
#define MT_MEAS_F_RMS_VOLTAGE    5  /* mV,  signed */
#define MT_MEAS_F_RMS_CURRENT    6  /* mA,  signed */
#define MT_ENERGY_F_IMPORTED     0  /* mWh, unsigned */
#define MT_ENERGY_F_EXPORTED     1  /* mWh, unsigned */

/*
 * AT+MTMEAS: push up to count (field, value) measurement pairs onto ep's
 * measurement cluster in one atomic call. cluster selects the family:
 *
 *   - ElectricalPowerMeasurement (0x0090): pull-model on the Matter side.
 *     Each value is stored into the endpoint's HearthEpmDelegate (main.cpp),
 *     where the CHIP Instance's attribute reads find it, and one
 *     MatterReportingAttributeChangeCallback per applied field makes
 *     subscriptions fire. Values are Nullable<int64_t>, null until the host
 *     first pushes them.
 *
 *   - ElectricalEnergyMeasurement (0x0091): push-model on the Matter side.
 *     The pairs (cumulative imported / exported mWh) are wrapped in
 *     EnergyMeasurementStruct with the timestamp policy the spec expects
 *     (endTimestamp = Matter epoch seconds when wall time is synced, else
 *     endSystime = milliseconds since boot; each push's start is the
 *     previous push's end, carried per endpoint by the SDK's own
 *     MeasurementDataForEndpoint storage) and handed to
 *     NotifyCumulativeEnergyMeasured(), which writes the attributes AND
 *     emits the CumulativeEnergyMeasured event in one call. A side the call
 *     does not mention keeps its previous stored value rather than being
 *     cleared to null.
 *
 *   - WaterHeaterManagement (0x0094, energy round B): pull-model like EPM.
 *     Each value is stored into the endpoint's HearthWhmDelegate (main.cpp),
 *     where the cluster Instance's attribute reads find it, one
 *     MatterReportingAttributeChangeCallback per applied field. Fields 3-5
 *     are feature-gated bridge-side (see the MT_WHM_F_* note below), and the
 *     firmware derives the cluster's events from BoostState transitions:
 *     Inactive to Active emits BoostStarted with the cached parameters of
 *     the last host-accepted Boost command (consumed on emission; duration 0
 *     and no optionals when there is none), Active to Inactive emits
 *     BoostEnded, and a push repeating the current state emits nothing.
 *
 *   - DeviceEnergyManagement (0x0098, energy round C1): pull-model like EPM
 *     and WHM. Each value is stored into the endpoint's HearthDemDelegate
 *     (main.cpp), one MatterReportingAttributeChangeCallback per applied
 *     attribute field, with two departures documented at MT_DEM_F_* below:
 *     ESAState pushes route through the delegate's SetESAState() (which owns
 *     the leaving-kPowerAdjustActive PowerAdjustEnd derivation and reports
 *     the attribute on change only, so a same-state push emits nothing and
 *     reports nothing), and field 6 is an event carrier that caches only and
 *     never reports anything dirty.
 *
 * Two-pass validate-then-apply, a hard contract: every pair is validated
 * before any pair is applied, so a bad third pair leaves the first two
 * unapplied and the device state untouched.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when cluster is none of the four push-served ids, ep
 * does not carry it, or (0x0094) a pushed field's backing feature is absent
 * on that endpoint, MT_ATTR_ERR_VALUE for a field unknown to that cluster or
 * a value outside that field's own XML range (see the field tables above and
 * below), MT_ATTR_ERR_FAILED for an internal failure (no delegate/storage
 * slot for ep, or the event emission itself failing).
 */
int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                       const int64_t *values, uint8_t count);

/*
 * The delegate-pool handout pattern (see mt_matter_valve_delegate_alloc()'s
 * doc comment above, which this mirrors), doubled: a measurement-capable
 * endpoint carries BOTH an ElectricalPowerMeasurement delegate and a
 * PowerTopology delegate, so the thunk (mt_devtypes.cpp, task 4) allocates
 * one of each, passes them through the two clusters' config.delegate fields
 * (cluster::create() wires the SDK's own init callbacks,
 * ElectricalPowerMeasurementDelegateInitCB / PowerTopologyDelegateInitCB,
 * esp_matter_delegate_callbacks.cpp, which construct and Init() the CHIP
 * Instances at the usual init-callback timing), calls create(), then fixes
 * the real endpoint id on both with mt_matter_meas_delegate_set_endpoint()
 * once it is known. The ElectricalEnergyMeasurement cluster needs no
 * delegate at all (free-function server); its per-endpoint init
 * (measurement accuracy, the one-time wildcard attribute-access
 * registration) is HearthEemInitCB (main.cpp), which the thunk hands to
 * esp_matter::cluster::set_delegate_and_init_callback() itself with a null
 * delegate pointer, the HearthOvenModeInitCB precedent.
 *
 * mt_matter_meas_delegate_set_endpoint() accepts a pointer from EITHER pool
 * (it recognises which pool the pointer came from), so the thunk needs no
 * second setter name. Both allocs return nullptr once MT_MEAS_MAX slots are
 * handed out, the same abort-the-boot-rebuild contract as every other pool
 * in this file.
 */
void *mt_matter_epm_delegate_alloc(void);
void *mt_matter_ptop_delegate_alloc(void);
void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep);

/* ---- water heater management (energy round B, task 1) ------------------- */

/*
 * How many WaterHeaterManagement-bearing endpoints one composition can
 * carry. Same deliberate step down from MT_COMP_MAX_ENDPOINTS as MT_MEAS_MAX
 * above, and the same reasoning: each slot is a HearthWhmDelegate (six cached
 * attribute values plus the cached Boost parameters plus vtable), and four
 * water heaters per composition is already beyond any host this firmware
 * targets.
 */
#define MT_WHM_MAX 4  /* WaterHeaterManagement endpoints per composition */

/*
 * AT+MTMEAS field ids for the WaterHeaterManagement cluster (0x0094), the
 * wire contract mt_matter_meas_set()'s 0x94 branch serves (mt_meas_whm_apply,
 * main.cpp), AT_MT_SPEC.md 3.25 documents, and the host library consumes.
 * Same family-selection rule as MT_MEAS_F_* / MT_ENERGY_F_* above:
 * the cluster id the host passes decides which table applies, so the spaces
 * may overlap numerically. Types and bounds follow the cluster XML exactly;
 * fields 3-4 exist only under the EnergyManagement feature and field 5 only
 * under TankPercent (a push to a field whose feature is absent answers the
 * same data-model code the energy fields answer on a power-only electrical
 * endpoint). Field 4 is int64 for 64-bit pipeline symmetry, but the XML
 * pins its minimum at 0, so a negative value is rejected as +MTERR:1.
 */
#define MT_WHM_F_HEATER_TYPES   0  /* bitmap8, unsigned */
#define MT_WHM_F_HEAT_DEMAND    1  /* bitmap8, unsigned */
#define MT_WHM_F_BOOST_STATE    2  /* enum8 0/1, unsigned */
#define MT_WHM_F_TANK_VOLUME    3  /* u16, unsigned, EM feature */
#define MT_WHM_F_EST_HEAT_REQ   4  /* int64 mWh, signed, min 0, EM feature */
#define MT_WHM_F_TANK_PERCENT   5  /* u8 0-100, unsigned, TP feature */

/*
 * The Boost forward's <mask> field (design spec 3.2): the +MTCMD line for
 * cluster 0x94 command 0 is "<duration>,<mask>[,<v1>[,<v2>[,<v3>]]]".
 * MT_BOOST_P_* are presence flags for the five optional Boost parameters in
 * canonical order; MT_BOOST_V_* carry the VALUES of the two bools when their
 * presence bit is set (a present bool needs no appended value field), and
 * are 0 when it is clear. The appended values are only the present NUMERIC
 * optionals, canonical order: temporarySetpoint (int16, temperature
 * hundredths), targetPercentage (u8), targetReheat (u8). Task 5 documents
 * this in AT_MT_SPEC.md 3.17; task 6's library unpacks with these same
 * names.
 */
#define MT_BOOST_P_ONESHOT    0x0001  /* oneShot present */
#define MT_BOOST_P_EMERGENCY  0x0002  /* emergencyBoost present */
#define MT_BOOST_P_SETPOINT   0x0004  /* temporarySetpoint present + appended */
#define MT_BOOST_P_TARGET_PCT 0x0008  /* targetPercentage present + appended */
#define MT_BOOST_P_REHEAT     0x0010  /* targetReheat present + appended */
#define MT_BOOST_V_ONESHOT    0x0100  /* oneShot's value when present */
#define MT_BOOST_V_EMERGENCY  0x0200  /* emergencyBoost's value when present */

/*
 * The WHM delegate-pool handout, task 2's thunk protocol. UNLIKE the
 * measurement pools above (alloc first, endpoint fixed later), this alloc
 * takes the endpoint id up front. Two reasons, both honest: the round's
 * task brief pins this alloc(ep) interface for tasks 2 and 3, and stamping
 * the id at handout makes task 3's by-endpoint slot lookup independent of
 * init-callback timing. The SDK does also set it, identically and later:
 * the Instance constructor calls mDelegate.SetEndpointId(aEndpointId)
 * (water-heater-management-server.h:142-148, the same as EPM) when
 * WaterHeaterManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:487-498) news the Instance. The thunk
 * calls this AFTER create() has returned the real id, attaching the
 * delegate via esp_matter::cluster::set_delegate_and_init_callback() with
 * the SDK's own WaterHeaterManagementDelegateInitCB (the HearthEemInitCB
 * post-create precedent: esp_matter invokes the callback whenever it is
 * set, esp_matter_data_model.cpp:264-266).
 *
 * Returns nullptr once MT_WHM_MAX slots are handed out, the same
 * abort-the-boot-rebuild contract as every other pool in this file. Task 3's
 * push bridge finds the slot again by endpoint id; task 2 consumes only this
 * alloc.
 */
void *mt_matter_whm_delegate_alloc(uint16_t ep);

/* ---- device energy management (energy round C1, task 1) ------------------ */

/*
 * How many DeviceEnergyManagement-bearing endpoints one composition can
 * carry. Same deliberate step down from MT_COMP_MAX_ENDPOINTS as MT_MEAS_MAX
 * and MT_WHM_MAX above, and the same reasoning: each slot is a
 * HearthDemDelegate (main.cpp), the largest pooled object yet (the cached
 * attribute values plus an owned PowerAdjustCapabilityStruct with a
 * MT_DEM_CAP_MAX_ENTRIES backing array). Sizing (design spec 2.1): battery
 * storage variant 0 and the standalone DEM device each take one in this
 * round's Phase 3, round C2's EVSE pair takes a third, headroom 1.
 */
#define MT_DEM_MAX 4  /* DeviceEnergyManagement endpoints per composition */

/*
 * AT+MTMEAS field ids for the DeviceEnergyManagement cluster (0x0098), the
 * wire contract task 2's 0x98 branch serves and AT_MT_SPEC.md documents.
 * Same family-selection rule as MT_MEAS_F_* / MT_WHM_F_* above: the cluster
 * id the host passes decides which table applies, so the spaces may overlap
 * numerically. Fields 0-5 back the cluster's six delegate-served scalar
 * attributes (all Instance-served, so pushes update the delegate cache and
 * report dirty; they never round-trip through ember). Field 6 is the odd one
 * out: AbsMinPower/AbsMaxPower's sibling numerically, but an EVENT CARRIER,
 * not an attribute. It caches the session's approximate energy use (mWh) for
 * the PowerAdjustEnd event's energyUse field, the reference delegate's
 * GetApproxEnergyDuringSession() shape, and reads back nothing.
 *
 * ESAState (field 2) transitions drive events (design spec 3.3): a push
 * LEAVING kPowerAdjustActive emits PowerAdjustEnd(NormalCompletion, measured
 * duration, cached field-6 energyUse); a same-state push emits nothing (the
 * Round B BoostState rule). The host does NOT push the entry transition: an
 * accepted PowerAdjustRequest sets kPowerAdjustActive firmware-side and
 * emits PowerAdjustStart itself (the contrast to Round B's Boost, where the
 * host pushed BoostState; here the event is fieldless and the state change
 * is unconditional on accept).
 */
#define MT_DEM_F_ESA_TYPE        0  /* enum8, unsigned */
#define MT_DEM_F_ESA_CAN_GEN     1  /* bool, unsigned */
#define MT_DEM_F_ESA_STATE       2  /* enum8, unsigned; transitions drive events */
#define MT_DEM_F_ABS_MIN_POWER   3  /* int64 mW, signed */
#define MT_DEM_F_ABS_MAX_POWER   4  /* int64 mW, signed */
#define MT_DEM_F_OPT_OUT_STATE   5  /* enum8, unsigned */
#define MT_DEM_F_ADJ_ENERGY_USE  6  /* int64 mWh, signed; EVENT CARRIER, not an attribute */

/*
 * How many PowerAdjustStruct entries the delegate's owned
 * PowerAdjustmentCapability list can hold: the AT+MTDEMCAP bound (design
 * spec 3.2, task 2's surface). The CHIP server validates every
 * PowerAdjustRequest against this list BEFORE the delegate runs
 * (device-energy-management-server.cpp:254-359), so until the host installs
 * entries the capability is null and every PowerAdjustRequest answers
 * ConstraintError without waking the host.
 */
#define MT_DEM_CAP_MAX_ENTRIES   4

/*
 * The DEM delegate-pool handout, task 3's thunk protocol. Same alloc(ep)
 * interface as mt_matter_whm_delegate_alloc() above and for the same two
 * reasons: the round's task brief pins it, and stamping the endpoint id at
 * handout makes the by-endpoint slot lookup independent of init-callback
 * timing (the SDK sets it again, identically and later: the Instance
 * constructor calls mDelegate.SetEndpointId(), device-energy-management-
 * server.h:210-216, when DeviceEnergyManagementDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:368-376) news the Instance from the
 * endpoint's FeatureMap snapshot). The thunk attaches the delegate via
 * esp_matter::cluster::set_delegate_and_init_callback() with the SDK's own
 * DeviceEnergyManagementDelegateInitCB, the WHM post-create precedent, and
 * sets DEM feature bits ONLY via cluster::device_energy_management::
 * feature::X::add() beforehand, never a FeatureMap write (the iron rule,
 * see HearthDemDelegate's class comment in main.cpp).
 *
 * Returns nullptr once MT_DEM_MAX slots are handed out, the same
 * abort-the-boot-rebuild contract as every other pool in this file.
 *
 * Consumed-by-task-2 entry points (all main.cpp-side, C++; listed here so
 * the names are pinned): task 2's mt_matter_meas_set() 0x98 branch finds the
 * slot with dem_for(ep) (the whm_for() shape), pushes cache fields through
 * the public members, drives ESAState transitions through
 * HearthDemDelegate::SetESAState() (the cluster Delegate virtual, overridden
 * to derive the PowerAdjustEnd(NormalCompletion) emission when a push leaves
 * kPowerAdjustActive), and caches field 6 through
 * HearthDemDelegate::SetAdjEnergyUse().
 *
 * Capability cause contract (task review F2, for task 2's AT+MTDEMCAP
 * bridge): the delegate keeps TWO cause values. The live one
 * (m_pa_capability.Value().cause) is firmware-owned while an adjustment
 * runs: an accepted PowerAdjustRequest stamps it with the request's
 * adjustment reason and every PowerAdjustEnd restores the baseline
 * (TC_DEM_2_2 steps 13a/14b assert the stamping). The bridge must set the
 * BASELINE member (m_pa_cause_baseline, default kNoAdjustment) and, only
 * when no adjustment is active, the live cause with it; see the store's
 * comment in main.cpp.
 */
void *mt_matter_dem_delegate_alloc(uint16_t ep);

/*
 * AT+MTDEMCAP: replace ep's whole DeviceEnergyManagement
 * PowerAdjustmentCapability (energy round C1 task 2, design spec 3.2).
 * quads carries n PowerAdjustStruct entries flattened in wire order,
 * 4 values each: minPower (int64 mW), maxPower (int64 mW), minDuration
 * (u32 s, already range-checked by cmd_mtdemcap), maxDuration (u32 s).
 * n is 0..MT_DEM_CAP_MAX_ENTRIES; 0 means the capability reads null (the
 * CHIP server then answers every PowerAdjustRequest ConstraintError
 * without waking the host, see MT_DEM_CAP_MAX_ENTRIES above).
 *
 * The bridge owns everything semantic (mt_at.c checks arity and
 * signedness only): the cause enum range
 * (0..PowerAdjustReasonEnum::kUnknownEnumValue-1), per-entry
 * minPower<=maxPower and minDuration<=maxDuration ordering (the CHIP
 * server's range walk assumes ordered entries,
 * device-energy-management-server.cpp:326-359), and the PowerAdjustment
 * feature gate (the attribute exists only under PA, XML mandatoryConform;
 * a variant-1 endpoint answers MT_ATTR_ERR_ATTRIBUTE). It honours the
 * capability cause contract above: the baseline member always takes
 * cause, the live struct cause only when no adjustment is running (a
 * running adjustment keeps its firmware-stamped live cause, restored to
 * the new baseline by the eventual PowerAdjustEnd). Replaces the owned
 * store under ChipStackLock and reports PowerAdjustmentCapability dirty.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT unknown ep,
 * MT_ATTR_ERR_CLUSTER no DeviceEnergyManagement cluster on ep,
 * MT_ATTR_ERR_ATTRIBUTE PowerAdjustment feature absent (variant 1),
 * MT_ATTR_ERR_VALUE cause out of enum range or an entry mis-ordered,
 * MT_ATTR_ERR_FAILED internal (no pool slot serves ep).
 */
int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads);

/* ---- nested row payloads (AT+MTROW family, energy round C2 task 2) ---- */

#include "mt_rows.h"

/*
 * Additional codes the mt_matter_rows_* bridge functions below may return,
 * beyond the four already defined in mt_rows.h (MT_ROW_OK, MT_ROW_ERR_FORM,
 * MT_ROW_ERR_VALUE, MT_ROW_ERR_KIND). mt_rows.h's codec is deliberately free
 * of any notion of a live endpoint or composition (its own file comment,
 * "free of IDF dependencies so the codec can be unit-tested on the host"),
 * so the two checks that DO need that knowledge, "does this endpoint
 * exist" and "does it carry a payload of this row kind", live here instead
 * of being folded into mt_rows.h's enum. Negative, and continuing that
 * enum's numbering, so a switch over every MT_ROW_* value stays one
 * enumeration for a caller that wants to.
 */
#define MT_ROW_ERR_ENDPOINT   (-4) /* unknown endpoint: mt_at.c answers +MTERR:2   */
#define MT_ROW_ERR_NO_PAYLOAD (-5) /* endpoint exists, no payload of this row kind:
                                     * mt_at.c answers +MTERR:4                    */
/*
 * The set was applied to the live data model but could not be written to
 * NVS, so it will not survive a reboot (energy round C2 task 4). Reported
 * rather than rolled back: a controller reading the payload back would
 * already see the new value, so "applied but not durable" is the only true
 * answer. mt_at.c answers +MTERR:7, the same MT_ERR_PERSIST every other NVS
 * failure in this firmware answers.
 */
#define MT_ROW_ERR_PERSIST    (-6)

/*
 * Commit a validated staged set for (ep, kind) (design spec 2.6, "AT+MTROW
 * family"). Called by cmd_mtrowapply() (mt_at.c) only after
 * mt_rows_validate() has already passed the set structurally; this function
 * owns everything that needs the live composition: whether <ep> exists,
 * whether it carries a cluster that stores payloads of <kind>, and actually
 * committing the data (kind 1, EVSE charging targets: MERGE by day bitmap
 * per EvseTargetsDelegate::SetTargets() semantics, not a wholesale replace,
 * design spec 2.6).
 *
 * stage->row[0..stage->count-1] are the rows to commit, but ONLY when
 * stage->active && stage->ep == ep && stage->kind == kind. When that match
 * does not hold, treat the call as applying zero rows, i.e. a clear: this
 * is the same (ep, kind) match convention mt_rows_clear() and
 * mt_rows_validate() already use against their own copy of the same
 * mt_row_stage_t, and it is what makes AT+MTROWAPPLY=<ep>,<kind>,0 with
 * nothing staged (the documented clear path, see mt_rows_validate()'s own
 * comment) work correctly: the caller passes the file-static stage
 * unconditionally and relies on this function to recognise it does not
 * belong to (ep, kind) rather than misreading unrelated staged data. Do
 * NOT read stage->row[] when the match fails.
 *
 * Returns MT_ROW_OK, MT_ROW_ERR_ENDPOINT (unknown ep), MT_ROW_ERR_NO_PAYLOAD
 * (ep exists, no cluster stores this row kind), MT_ROW_ERR_PERSIST (applied
 * but not saved), or MT_ROW_ERR_VALUE for a semantic rejection only the live
 * model can see (e.g. CHIP's own day-bit reuse check running against the
 * MERGED result, not just the applied set; for kind 1, more than seven
 * distinct day bitmaps or more than ten targets sharing one, neither of
 * which the row codec knows about because neither is a property of a
 * row). Must hold ChipStackLock, the standing mt_matter_* bridge convention
 * (every bridge function that touches the CHIP stack takes it; see the
 * project's "Every mt_matter_* bridge function must hold the CHIP stack
 * lock" note).
 *
 * Task 2 ships a weak, fail-closed stub in mt_at.c (always
 * MT_ROW_ERR_ENDPOINT) purely so the firmware links before the real
 * implementation lands; whichever task wires kind 1 to a live store
 * provides the strong definition here and the stub silently stops being
 * linked in.
 */
int mt_matter_rows_apply(uint16_t ep, uint8_t kind, const mt_row_stage_t *stage);

/*
 * Read one stored row. On MT_ROW_OK, *out is filled (out->nfields set to
 * the kind's field count, out->present[]/value[] to the stored row) and
 * *total is set to the endpoint's stored row count for <kind>; *total is
 * only meaningful when the return is MT_ROW_OK.
 *
 * The caller (cmd_mtrowget(), mt_at.c) always calls mt_matter_rows_total()
 * first and only calls this function for idx < that total (design spec
 * 2.3, "reads are stateless"): idx is therefore guaranteed in range by the
 * time this runs, and this function does not need its own "idx past the
 * end" case. It still independently reports *total on every successful
 * call rather than trusting the caller's earlier snapshot, since the two
 * calls are not atomic with each other.
 *
 * Returns MT_ROW_OK, MT_ROW_ERR_ENDPOINT (unknown ep) or
 * MT_ROW_ERR_NO_PAYLOAD (ep exists, no payload of this kind). Must hold
 * ChipStackLock, same convention as mt_matter_rows_apply() above.
 *
 * Task 2's weak stub (mt_at.c) always answers MT_ROW_ERR_ENDPOINT and
 * leaves *out untouched, *total set to 0.
 */
int mt_matter_rows_get(uint16_t ep, uint8_t kind, uint16_t idx,
                       mt_row_t *out, uint16_t *total);

/*
 * Number of stored rows for (ep, kind): AT+MTROWGET's bulk-read loop bound,
 * and the first check cmd_mtrowget() makes so the +MTERR:2 / +MTERR:4 cases
 * are answered before any row read is attempted. *total is set to 0 on
 * every error return and to the true count (0 is a valid, empty payload)
 * on MT_ROW_OK.
 *
 * Returns MT_ROW_OK, MT_ROW_ERR_ENDPOINT or MT_ROW_ERR_NO_PAYLOAD, same
 * division as mt_matter_rows_get() above. Must hold ChipStackLock, same
 * convention as mt_matter_rows_apply() above.
 *
 * Task 2's weak stub (mt_at.c) always answers MT_ROW_ERR_ENDPOINT and
 * leaves *total set to 0.
 */
int mt_matter_rows_total(uint16_t ep, uint8_t kind, uint16_t *total);

/* ---- Energy EVSE / Electrical Utility Meter pools (energy round C2, task 3) --- */

/*
 * How many Energy EVSE endpoints one composition can carry. Task 4 sizes
 * HearthEvseDelegate's pool by this constant (main.cpp); this task only
 * reserves the name so mt_devtypes.cpp's thunk can be reviewed against a
 * pool that is already accounted for in mt_matter.h, the same order C1's
 * MT_DEM_MAX shipped in task 1 ahead of task 2's delegate. Headroom over the
 * one EVSE endpoint Phase 3 adds, the C1 policy of deliberate headroom
 * rather than an exact fit (B247).
 */
#define MT_EVSE_MAX 2  /* EnergyEvse endpoints per composition */

/*
 * How many Electrical Utility Meter endpoints one composition can carry.
 * Task 8/9 size the MeterIdentification Instance pool this constant bounds
 * (main.cpp), the same "name reserved ahead of the consumer" shape as
 * MT_EVSE_MAX above. Headroom over the one meter endpoint Phase 3 adds.
 */
#define MT_METER_MAX 2  /* Electrical Utility Meter endpoints per composition */

/* ---- Energy EVSE delegate and targets store (energy round C2, task 4) ---- */

/*
 * The EVSE delegate-pool handout, mt_devtypes.cpp's thunk protocol, and the
 * mt_matter_whm_delegate_alloc() shape exactly: the endpoint id is taken at
 * handout, and the thunk calls this AFTER energy_evse::create() has returned
 * the real id, attaching the result with
 * esp_matter::cluster::set_delegate_and_init_callback() and the SDK's own
 * EnergyEvseDelegateInitCB.
 *
 * The attach must come AFTER the thunk's feature::add() calls, not through
 * config.energy_evse.delegate before create(): EnergyEvseDelegateInitCB
 * (esp_matter_delegate_callbacks.cpp:257-268) snapshots the endpoint's
 * FeatureMap when it news the Instance, and that snapshot is what makes
 * Instance::ValidateTargets() demand a targetSoC in every charging target on
 * a variant-0 (SOC) endpoint. A delegate attached before the SOC bit is
 * added would be Instance'd against a FeatureMap without it.
 *
 * Allocation also LOADS the endpoint's stored schedule from NVS, which is
 * how the SDK's unenforced "LoadTargets() must be called before
 * GetTargets()" contract is satisfied on this firmware (mt_evse.cpp's file
 * comment carries the citation).
 *
 * Returns nullptr once MT_EVSE_MAX slots are handed out, the same
 * abort-the-boot-rebuild contract as every other pool in this file.
 */
void *mt_matter_evse_delegate_alloc(uint16_t ep);

/*
 * AT+MTMEAS field ids for the EnergyEvse cluster (0x0099), the wire contract
 * task 7's 0x99 branch serves. Same family-selection rule as MT_MEAS_F_* /
 * MT_WHM_F_* / MT_DEM_F_* above: the cluster id the host passes decides
 * which table applies, so the spaces may overlap numerically.
 *
 * Every EnergyEvse attribute is served by the cluster's Instance from the
 * delegate's cache, so this push is the ONLY way a host can set any of them
 * and none of them ever raises a +MTATTR URC. Three of them
 * (UserMaximumChargeCurrent, RandomizationDelayWindow,
 * ApproximateEVEfficiency) are additionally writable from the fabric, and
 * routing the host side through here rather than AT+MTATTR is a deliberate
 * avoidance of DE270 (a write to a delegate-served attribute over AT+MTATTR
 * collapses to a bare ERROR because the SDK discards the provider's status),
 * not a fix for it.
 *
 * A field whose attribute is absent from the endpoint answers
 * MT_ATTR_ERR_ATTRIBUTE rather than being silently cached: fields 14/15 on a
 * variant-1 (no SOC) endpoint, and today also fields 7, 8 and 13, whose
 * attributes esp-matter declares creators for and never calls (see
 * mt_evse.cpp).
 */
#define MT_EVSE_F_STATE                   0  /* enum8 0..6,  unsigned */
#define MT_EVSE_F_SUPPLY_STATE            1  /* enum8 0..5,  unsigned */
#define MT_EVSE_F_FAULT_STATE             2  /* enum8 0..15, unsigned */
#define MT_EVSE_F_CHARGING_ENABLED_UNTIL  3  /* u32 epoch-s, unsigned, nullable */
#define MT_EVSE_F_CIRCUIT_CAPACITY        4  /* int64 mA, signed, min 0 */
#define MT_EVSE_F_MIN_CHARGE_CURRENT      5  /* int64 mA, signed, min 0 */
#define MT_EVSE_F_MAX_CHARGE_CURRENT      6  /* int64 mA, signed, min 0 */
#define MT_EVSE_F_USER_MAX_CHARGE_CURRENT 7  /* int64 mA, signed, min 0, optional attr */
#define MT_EVSE_F_RANDOMIZATION_WINDOW    8  /* u32 s 0..86400, unsigned, optional attr */
#define MT_EVSE_F_NEXT_CHARGE_START       9  /* u32 epoch-s, unsigned, nullable, PREF */
#define MT_EVSE_F_NEXT_CHARGE_TARGET     10  /* u32 epoch-s, unsigned, nullable, PREF */
#define MT_EVSE_F_NEXT_CHARGE_ENERGY     11  /* int64 mWh, signed, min 0, nullable, PREF */
#define MT_EVSE_F_NEXT_CHARGE_SOC        12  /* u8 0..100, unsigned, nullable, PREF */
#define MT_EVSE_F_APPROX_EFFICIENCY      13  /* u16, unsigned, nullable, optional attr */
#define MT_EVSE_F_STATE_OF_CHARGE        14  /* u8 0..100, unsigned, nullable, SOC */
#define MT_EVSE_F_BATTERY_CAPACITY       15  /* int64 mWh, signed, min 0, nullable, SOC */
#define MT_EVSE_F_SESSION_ID             16  /* u32, unsigned, nullable */
#define MT_EVSE_F_SESSION_DURATION       17  /* u32 s, unsigned, nullable */
#define MT_EVSE_F_SESSION_ENERGY_CHARGED 18  /* int64 mWh, signed, min 0, nullable */

/*
 * Push ONE EnergyEvse attribute onto ep, the single-pair form of the
 * measurement-push contract (task 7 adds the multi-pair AT+MTMEAS branch on
 * top of the same code path, so the two cannot drift). Validates before it
 * applies and reports the attribute dirty on success.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT unknown ep,
 * MT_ATTR_ERR_CLUSTER ep carries no EnergyEvse cluster, MT_ATTR_ERR_ATTRIBUTE
 * the field's attribute is not present on this endpoint (wrong variant, or
 * one of the three esp-matter never creates), MT_ATTR_ERR_VALUE unknown field
 * id or a value outside that field's XML range, MT_ATTR_ERR_FAILED internal
 * (no pool slot serves ep). Holds ChipStackLock.
 */
int mt_matter_evse_set(uint16_t ep, uint8_t field, int64_t value);

/*
 * Commit a staged charging-target set (row kind 1) as ep's schedule, MERGING
 * by day: days present in the applied set replace those days entirely, days
 * absent are unchanged, and a stage that does not match (ep, kind) or carries
 * no rows clears the whole stored schedule. See mt_matter_rows_apply() above
 * for the shared contract and mt_evse.cpp for why the merge is not a
 * wholesale replace.
 *
 * stage points at mt_at.c's file-static staging buffer, not a copy (the
 * struct is several KB and the AT parser task's stack is 6144 bytes); it is
 * consumed before the call returns and the pointer is never retained.
 *
 * Task 5 routes mt_matter_rows_apply()'s kind-1 arm here. Returns an MT_ROW_*
 * code. Holds ChipStackLock.
 */
int mt_matter_evse_targets_apply(uint16_t ep, const mt_row_stage_t *stage);

/*
 * Read stored row idx of ep's charging schedule, and ep's total stored row
 * count. Rows are the flattened (schedule, target) sequence in stored order;
 * out->present[] carries the absent-optional convention (SoC and added energy
 * are each optional, at least one is always present). Task 5 routes
 * mt_matter_rows_get()'s kind-1 arm here. Returns an MT_ROW_* code, plus
 * MT_ROW_ERR_VALUE if idx is at or past the total. Holds ChipStackLock.
 */
int mt_matter_evse_targets_get(uint16_t ep, uint16_t idx, mt_row_t *out, uint16_t *total);

/*
 * Number of stored rows for ep, AT+MTROWGET's bulk-loop bound and the check
 * that answers +MTERR:2 / +MTERR:4 before any row is read. Task 5 routes
 * mt_matter_rows_total()'s kind-1 arm here. MT_ROW_* code, ChipStackLock.
 */
int mt_matter_evse_targets_total(uint16_t ep, uint16_t *total);

/*
 * Erase every stored charging schedule, RAM and NVS, for AT+MTFRESET only.
 * The schedule is USER DATA: it survives AT+MTRESET and a power cycle, and
 * only a factory reset removes it. That is the exact opposite of the
 * composition, which survives AT+MTFRESET's Matter reset half because it is a
 * product definition rather than something an end user chose; the two live in
 * the same NVS partition with deliberately opposite lifetimes.
 *
 * Returns 0 on success, -1 if the NVS erase failed (the RAM stores are
 * cleared either way, and the caller reboots immediately after).
 */
int mt_matter_evse_targets_erase_all(void);

#ifdef __cplusplus
}
#endif
