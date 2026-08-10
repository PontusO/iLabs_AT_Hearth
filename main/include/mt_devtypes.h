/*
 * mt_devtypes.h - map a Matter device type ID to its esp_matter endpoint
 * constructor.
 *
 * C-linkage bridge: the table itself is C++ because every esp_matter endpoint
 * namespace is, but mt_at.c and the boot path reach it through plain C.
 *
 * Device type IDs are read from esp_matter's own get_device_type_id() rather
 * than transcribed, so the table cannot drift from the SDK we build against.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when devtype_id appears in the table. */
bool mt_devtype_is_known(uint32_t devtype_id);

/*
 * True when variant is a legal variant for devtype_id: 0 for every device
 * type that has no variants of its own, 0..max_variant for one that does
 * (e.g. the temperature-controlled cabinet's 0 = number, 1 = level). False
 * for an unknown devtype_id too, so a caller need not check is_known() first,
 * though mt_at.c's +MTERR division still distinguishes the two: unknown ID
 * is MT_ERR_DEVTYPE, known ID with a bad variant is MT_ERR_BAD_PARAM.
 */
bool mt_devtype_variant_ok(uint32_t devtype_id, uint8_t variant);

/*
 * True when parent_devtype is a legal parent for an endpoint of devtype_id
 * (variant currently unused by any rule, carried for symmetry with the rest
 * of the table). parent_devtype 0 means "no parent" (0 is not a Matter
 * device type id), and every device type accepts that unless it specifically
 * requires a parent. This is the append-time policy: mt_at.c's cmd_mtep()
 * calls it both when a parent index is given and, with parent_devtype 0,
 * when one is not, so a device type that REQUIRES a parent is rejected in
 * the unparented case too.
 */
bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype);

/*
 * Create one endpoint of the given device type and variant on the current
 * node and write its assigned endpoint ID to *out_ep_id. When parent_devtype
 * is nonzero, parents the new endpoint under parent_ep_id (an already-created
 * endpoint's id) via endpoint::set_parent_endpoint(); parent_devtype 0 means
 * no parenting.
 *
 * Must be called after node::create() and before esp_matter::start(): esp_matter
 * persists its endpoint-id counter only for endpoints created after start, so
 * creating before start is what makes the ids reproducible across boots. See
 * the design spec sections 5.3 and 12.1.
 *
 * Returns 0 on success, -1 on an unknown device type, a creation failure, or
 * a failure to set the parent (the caller aborts the whole rebuild in that
 * case, same as any other creation failure: see main.cpp's rebuild loop).
 */
int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                       uint16_t parent_ep_id, uint16_t *out_ep_id);

#ifdef __cplusplus
}
#endif
