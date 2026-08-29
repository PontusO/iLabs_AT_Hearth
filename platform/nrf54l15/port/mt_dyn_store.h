/*
 * mt_dyn_store.h - port-internal handle on the dynamic endpoints' external
 * attribute store (Matter core spec section 5).
 *
 * Every attribute on a dynamic endpoint is declared EXTERNAL_STORAGE (the
 * DECLARE_DYNAMIC_ATTRIBUTE macro sets that flag unconditionally), so CHIP
 * keeps no value bytes of its own for them: the values live in
 * mt_devtypes_zephyr.cpp's per-endpoint blocks, one heap allocation per
 * created endpoint holding that endpoint's DataVersion array and its
 * attribute slots, sized for its own device type. This header is how the
 * rest of the port reaches those blocks; it is C++ only and never leaves
 * platform/nrf54l15/port.
 *
 * No external consumer exists yet: Task 5's mt_matter_attr_read/_write
 * bridge went through the ember path this header's own comment below
 * prefers, exactly as intended, and nothing else in this port has needed
 * mt_dyn_attr_slot() so far. That is not a reason to fold this header
 * away. It stays as the deliberate contract for a future round that
 * genuinely cannot go through emberAfReadAttribute()/emberAfWriteAttribute()
 * (an SDK callback that already holds the ember lookup, say), so that
 * round has a documented, lock-audited entry point instead of walking the
 * header table and following block pointers ad hoc.
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
 * bytes) and the function returns true. The pointer aliases into that
 * endpoint's heap block, which is allocated once at boot and never freed,
 * so it stays valid for the life of the boot subject to the locking rules
 * above. Returns false when the endpoint is not a live dynamic endpoint, or
 * when the attribute has no slot: that is
 * the case for every Descriptor attribute and for anything ARRAY-typed,
 * which CHIP serves from its own cluster objects rather than from here.
 */
bool mt_dyn_attr_slot(chip::EndpointId ep, chip::ClusterId cluster, chip::AttributeId attr,
                      uint8_t **data, uint8_t *size);
