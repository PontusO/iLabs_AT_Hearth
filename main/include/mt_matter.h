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

#ifdef __cplusplus
}
#endif
