/*
 * mt_matter.h - C-linkage bridge from the AT+MT command handlers (mt_at.c, C)
 * into the esp_matter / CHIP runtime (implemented in main.cpp, C++).
 *
 * Keeps all C++ (esp_matter/CHIP) out of the C command-handler translation
 * unit: mt_at.c calls these plain-C wrappers, main.cpp implements them.
 */

#pragma once

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

/* ---- live composition (built at boot from the stored composition) ------ */

/* Number of endpoints this device currently presents, excluding the Root
 * Node on endpoint 0. Zero means unconfigured (design spec section 5.5). */
uint16_t mt_matter_endpoint_count(void);

/*
 * Describe the index'th live endpoint in creation order. Writes its Matter
 * device type ID and assigned endpoint ID. Returns 0 on success, -1 when
 * index is out of range.
 */
int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id);

/* Record an endpoint in the live table as the boot rebuild creates it. */
void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id);

/* Read an integer-valued attribute into *out. Returns 0 on success, -1 on
 * failure (not found, or a non-integer type). */
int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, long *out);

/* Write an integer-valued attribute (interpreted per the attribute's current
 * type). Returns 0 on success, -1 on failure. */
int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, long val);

#ifdef __cplusplus
}
#endif
