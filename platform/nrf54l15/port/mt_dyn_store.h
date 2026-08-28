/*
 * mt_dyn_store.h - port-internal handle on the dynamic endpoints' external
 * attribute store (Matter core spec section 5).
 *
 * Every attribute on a dynamic endpoint is declared EXTERNAL_STORAGE (the
 * DECLARE_DYNAMIC_ATTRIBUTE macro sets that flag unconditionally), so CHIP
 * keeps no value bytes of its own for them: the values live in
 * mt_devtypes_zephyr.cpp's per-endpoint slot arena. This header is how the
 * rest of the port reaches that arena; it is C++ only and never leaves
 * platform/nrf54l15/port.
 */

#pragma once

#include <lib/core/DataModelTypes.h>

#include <stdint.h>

/*
 * Threading and reporting contract
 * --------------------------------
 * PREFERRED: do not call this function to read or write attribute values.
 * Go through emberAfReadAttribute() / emberAfWriteAttribute(), which reach
 * this same store through the ember external-storage callbacks in
 * mt_devtypes_zephyr.cpp. That path does two things this function cannot:
 * it validates against the attribute metadata, and it raises the
 * attribute-changed notification, so subscriptions and bindings observe the
 * new value. A value poked in through mt_dyn_attr_slot() changes what a
 * later read returns and nothing else: every existing subscriber keeps
 * reporting the old one. Task 5's mt_matter_attr_read/_write bridge uses
 * the ember path for exactly this reason.
 *
 * This function is for the cases the ember path cannot serve. Whoever calls
 * it:
 *   - must hold chip::DeviceLayer::StackLock unless already running on the
 *     CHIP thread, since the returned pointer aliases live storage that
 *     CHIP itself reads and writes;
 *   - must not keep the returned pointer past releasing that lock, and must
 *     not assume the endpoint is still live across a release;
 *   - owns raising the attribute-changed notification after any write.
 *
 * Look up one attribute's value bytes on a dynamic endpoint. On success
 * *data points at the live storage (writable, little-endian, exactly *size
 * bytes) and the function returns true. Returns false when the endpoint is
 * not a live dynamic endpoint, or when the attribute has no slot: that is
 * the case for every Descriptor attribute and for anything ARRAY-typed,
 * which CHIP serves from its own cluster objects rather than from here.
 */
bool mt_dyn_attr_slot(chip::EndpointId ep, chip::ClusterId cluster, chip::AttributeId attr,
                      uint8_t **data, uint8_t *size);
