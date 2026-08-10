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
} mt_attr_result_t;

/* Read an integer-valued attribute into *out. Returns an mt_attr_result_t. */
int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, long *out);

/*
 * Write an integer-valued attribute (interpreted per the attribute's current
 * type). Returns an mt_attr_result_t.
 *
 * notify=true  uses attribute::update(): subscribers and bound devices see the
 *              change, which is what a host-driven change should normally do.
 * notify=false uses attribute::set_val(): the value changes locally without a
 *              report. Used when reflecting a change that came FROM a
 *              controller, so echoing it back does not loop.
 */
int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, long val, bool notify);

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
 */
#define MT_MB_MAX_LISTS     8   /* (endpoint, cluster) lists this firmware can store */
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
 * header. cluster must be one of the three ModeBase cluster ids
 * (RvcRunMode, RvcCleanMode, MicrowaveOvenMode); this bridge is the sole
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
 * kNormal on every mode. Substituting at store time rather than in the
 * delegate's GetModeTagsByIndex() keeps every read branch-free. A nonzero
 * tag passes through unvalidated beyond the u16 range check mt_at.c already
 * performed: tag semantics beyond that are the host's business.
 *
 * Also clamps CurrentMode when the replacement list drops the value it
 * currently holds (main.cpp), through the SDK's own
 * ModeBase::Instance::UpdateCurrentMode(), so a controller never observes a
 * CurrentMode that is not a member of the just-published SupportedModes.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when cluster is not one of the three ModeBase ids, or
 * ep has no such cluster, MT_ATTR_ERR_FAILED for a bad count or an internal
 * failure (store exhaustion: MT_MB_MAX_LISTS (8) is smaller than
 * MT_COMP_MAX_ENDPOINTS (16), so a composition with enough ModeBase cluster
 * instances CAN exhaust it; refused with this code rather than silently
 * overwriting an unrelated (endpoint, cluster) slot).
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
 * mt_matter_opstate_delegate_alloc() returns nullptr once
 * MT_COMP_MAX_ENDPOINTS slots are handed out, so the caller aborts the boot
 * rebuild the same way a failed create() itself would (see
 * mt_matter_valve_delegate_alloc()'s doc comment above for the full
 * reasoning).
 */
void *mt_matter_opstate_delegate_alloc(void);
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
 * mt_at.c's cmd_mtopstate rejects anything outside the UNION of both sets
 * with +MTERR:1 before this is ever called (it cannot know which cluster ep
 * actually has); this bridge is what narrows that down to the cluster-
 * specific legal set, answering MT_ATTR_ERR_VALUE (+MTERR:1) for a state
 * that is in the union but not legal for ep's own cluster - e.g. 0x40 on a
 * plain washer endpoint. 3 (Error) is outside the union entirely and is
 * rejected by mt_at.c itself for both cluster kinds: kError is reserved for
 * the error-detection path (F7), never a state this command may set
 * directly; SetOperationalState() enforces the identical rule independently
 * (see main.cpp), so a state that somehow slipped past both checks would
 * still be refused there, not silently accepted.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has neither OperationalState-family cluster,
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

/* ---- smoke/co alarm (seven-type batch, task C5) -------------------------- */

/*
 * AT+MTALARM: set field <field> (1..11) of ep's SmokeCoAlarm cluster through
 * one of the cluster's own eleven event-emitting Set* methods
 * (SmokeCoAlarmServer::Instance(), design spec F4), never a raw AT+MTATTR
 * write: the setters fire the cluster's spec-mandated events (SmokeAlarm,
 * COAlarm, MuteEnded, HardwareFault, EndOfService, AllClear, LowBattery,
 * SelfTestComplete) and the critical-alarm auto-unmute, both of which a raw
 * write to the same ember-managed attribute storage would silently skip.
 *
 * mt_at.c's cmd_mtalarm has already rejected field outside 1..11 with
 * +MTERR:1 before this is ever called (field 0, ExpressedState, is derived
 * by the server from the other ten and is never settable directly, so it is
 * not in that range at all). This bridge maps field to the matching setter
 * and range-checks <value> against THAT field's own SDK enum bound (each
 * setter takes a differently-typed enum, so mt_at.c, plain C with no SDK
 * access, cannot do this itself); the two boolean fields (TestInProgress,
 * HardwareFaultAlert) are checked against 0/1 instead. An out-of-range value
 * returns MT_ATTR_ERR_VALUE (+MTERR:1, the C1b/B139 precedent); a setter
 * returning false (the field already held that value, or the underlying
 * ember write failed) maps to MT_ATTR_ERR_FAILED, a bare ERROR.
 *
 * Field 5 (TestInProgress) value 0 is the self-test completion path: the
 * SDK's own SetTestInProgress() recognises the true->false edge and fires
 * SelfTestComplete, the far end of the notify-only +MTCMD:0,... a
 * controller's SelfTestRequest raises (mt_cmd_notify(), C1; see
 * emberAfPluginSmokeCoAlarmSelfTestRequestCommand() in main.cpp).
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no SmokeCoAlarm cluster, MT_ATTR_ERR_VALUE
 * for a <value> outside the field's own range, MT_ATTR_ERR_FAILED when the
 * setter itself reports failure.
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

#ifdef __cplusplus
}
#endif
