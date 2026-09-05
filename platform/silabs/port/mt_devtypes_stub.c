/*
 * mt_devtypes_stub.c - the mt_devtypes.h quartet for the Silabs skeleton.
 * Accept-all predicates and a create() that refuses, so AT+MTEP grammar can
 * be exercised end to end before any device type exists on this platform
 * (the nRF skeleton took the same shape). Replaced by the real registry in
 * the upward-port round.
 */

#include <stddef.h>

#include "mt_devtypes.h"

bool mt_devtype_is_known(uint32_t devtype_id)
{
    (void)devtype_id;
    return true;
}

bool mt_devtype_variant_ok(uint32_t devtype_id, uint8_t variant)
{
    (void)devtype_id;
    (void)variant;
    return true;
}

bool mt_devtype_parent_ok(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype)
{
    (void)devtype_id;
    (void)variant;
    (void)parent_devtype;
    return true;
}

int mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint32_t parent_devtype,
                      uint16_t parent_ep_id, uint16_t *out_ep_id)
{
    (void)devtype_id;
    (void)variant;
    (void)parent_devtype;
    (void)parent_ep_id;
    if (out_ep_id != NULL) *out_ep_id = 0;
    return -1; /* no endpoints exist on the skeleton */
}
