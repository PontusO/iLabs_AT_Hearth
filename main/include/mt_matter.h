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
 * device type ID, assigned endpoint ID and composition variant. Returns 0 on
 * success, -1 when index is out of range.
 */
int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id, uint8_t *variant);

/* Record an endpoint in the live table as the boot rebuild creates it. */
void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant);

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
 * AT+MTOPSTATE: set ep's OperationalState cluster (0x0060) CurrentState
 * through the SDK's own Instance::SetOperationalState(), so the
 * OperationalState attribute report a subscribed controller expects is
 * actually emitted (main.cpp's HearthOpStateDelegate class comment has the
 * full F7 citation trail, including why this needs no registry beyond the
 * delegate pool itself). Same split-ownership shape as AT+MTLOCK/AT+MTVALVE:
 * the firmware never calls this on its own after an allowed +MTCMD verdict
 * (Pause/Resume/Start/Stop, cluster 0x0060 commands 0-3), since only the
 * host knows when the physical appliance has actually completed the
 * transition.
 *
 * <state> is an OperationalStateEnum wire value: 0 Stopped, 1 Running, 2
 * Paused. mt_at.c rejects 3 (Error) itself before calling this, since kError
 * is reserved for the error-detection path (F7), never a state this command
 * may set directly; SetOperationalState() enforces the identical rule
 * independently (see main.cpp), so a state that somehow slipped past the
 * handler would still be refused here, not silently accepted.
 *
 * Returns an mt_attr_result_t: MT_ATTR_ERR_ENDPOINT for an unknown ep,
 * MT_ATTR_ERR_CLUSTER when ep has no OperationalState cluster,
 * MT_ATTR_ERR_FAILED when no delegate/Instance is reachable for ep (should
 * not happen once esp_matter::start() has run, see main.cpp) or
 * SetOperationalState() itself reports failure.
 */
int mt_matter_opstate_set(uint16_t ep, uint8_t state);

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

#ifdef __cplusplus
}
#endif
