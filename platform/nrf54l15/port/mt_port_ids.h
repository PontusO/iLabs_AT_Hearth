/*
 * mt_port_ids.h - numeric ids shared between the nRF54L15 port's own
 * translation units, so a value like the catalogue endpoint id is named
 * once instead of risking two independently-typed literals drifting
 * apart.
 */

#pragma once

#include <stdint.h>

/*
 * The catalogue endpoint exists only to compile cluster server code into
 * the image (spec section 3); it is never visible on the fabric.
 * main.cpp disables it at boot (emberAfEndpointEnableDisable); the same
 * id is why mt_matter_zephyr.cpp's MatterPostAttributeChangeCallback()
 * skips it, so a write to catalogue-endpoint storage never turns into a
 * +MTATTR URC for an endpoint the host was never told exists.
 */
constexpr uint16_t kCatalogueEndpointId = 240;

/*
 * Endpoint ACCEPTANCE and endpoint CAPACITY are two different numbers, and
 * this constant is the second one.
 *
 * Acceptance is MT_COMP_MAX_ENDPOINTS (core/include/mt_composition.h, 28):
 * how many endpoints the AT wire contract lets a host DECLARE over AT+MTEP.
 * That is a core constant, part of the contract both platforms share, and it
 * does not move because one platform is smaller than the other.
 *
 * Capacity is this: how many of those endpoints THIS build can stand up and
 * serve concurrently. It sizes two very different things at once, which is
 * why lowering it pays twice:
 *
 *   - The port's own dynamic endpoint header table (mt_devtypes_zephyr.cpp's
 *     s_dyn), now a slim header array rather than a flat slot arena.
 *   - Every compile-time per-endpoint pool inside CHIP, through
 *     CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT, which
 *     src/chip_project_config.h mirrors from this value. ColorControlServer
 *     alone carries THIRTEEN arrays sized fixed + dynamic count, not the
 *     eleven this comment claimed before memory reclaim round A counted
 *     them: five under MATTER_DM_PLUGIN_COLOR_CONTROL_SERVER_HSV
 *     (color-control-server.h:301-306), four under _XY (:310-314), two
 *     under _TEMP (:318-319) and two unconditional (:322-323), with all
 *     three plugin variants defined in this build's gen_config.h:614-616.
 *     264 B per endpoint, so 4,488 B of the 4,496 B
 *     ColorControlServer::instance symbol is per-endpoint array, measured
 *     with nm on 2026-08-30. Level-control's state table, BooleanState's
 *     server pool and the Thermostat / WindowCovering / FanControl delegate
 *     tables are all sized the same way.
 *
 * 16 rather than 28 is a deliberate trade for an nRF54L15's 256 KB. A host
 * may still DECLARE up to 28 endpoints and the composition still persists
 * intact; a composition longer than 16 fails its rebuild at the seventeenth
 * endpoint, loudly. That abort is stop-at-failure, not roll-back
 * (AT_MT_SPEC.md 501-506), so the first sixteen stay live as a prefix with
 * unchanged ids and the rest are absent: a twenty-endpoint composition
 * serves 1..16. The nRF54LM20 tier (512 KB, supported
 * upstream in this NCS) is where this goes back up: raising this constant
 * and HEARTH_EP_HEAP_BYTES below is the whole change.
 *
 * mt_devtypes_zephyr.cpp static_asserts both halves of the split: that
 * chip_project_config.h still mirrors this number, and that capacity never
 * exceeds acceptance.
 */
constexpr uint16_t kServiceableEndpoints = 16;

/*
 * Bytes reserved for the endpoint block heap (mt_devtypes_zephyr.cpp's
 * K_HEAP_DEFINE(hearth_ep_heap)). One block per created endpoint holds that
 * endpoint's DataVersion array and attribute slots, sized for its device
 * type rather than for the widest type in the catalogue.
 *
 * Sized for the realistic worst case, NOT for 16 copies of the heaviest
 * device type. The arithmetic sits beside the heap definition in
 * mt_devtypes_zephyr.cpp and in the platform README's "Endpoint capacity"
 * section; the short version is that 8 KB holds 16 of every catalogue device
 * type except the two colour lights, 15 colour temperature lights, or 13
 * extended colour lights, against a 16-extended-colour-light worst case that
 * would want 9,600 B. A composition asking for more fails loudly at the
 * endpoint that does not fit.
 *
 * Raised together with kServiceableEndpoints for the LM20 tier.
 */
#define HEARTH_EP_HEAP_BYTES 8192

/*
 * Bytes reserved for the cluster-object heap (mt_matter_zephyr.cpp's
 * K_HEAP_DEFINE(hearth_obj_heap)). It holds the per-endpoint CHIP Delegate
 * objects and the raw storage for their Instances: the fourteen pools that
 * used to be fixed .bss and .data arrays, one slot per endpoint that COULD
 * carry the family, whatever the host actually composed.
 *
 * A separate heap from hearth_ep_heap deliberately. The endpoint block heap
 * hands out 4-byte-aligned blocks (k_heap_alloc routes through
 * sys_heap_noalign_alloc) and its static_asserts exist to keep anything
 * wider out; HearthDemDelegate holds a uint64_t and would break that
 * guarantee. This heap allocates through k_heap_aligned_alloc() at 8 bytes
 * throughout, so alignment is a property of the heap rather than of each
 * caller.
 *
 * Sized so it can NEVER be the binding wall: every composition the existing
 * walls (kServiceableEndpoints, HEARTH_EP_HEAP_BYTES and the per-family
 * MT_*_MAX caps) already admit fits. The arithmetic sits beside the heap
 * definition in mt_matter_zephyr.cpp, where the maximising composition is
 * found by EXHAUSTIVE search over the catalogue rather than by a greedy
 * fill (the fix round's C1: a greedy fill missed the true maximum by
 * 224 B), and is pinned by a static_assert there, so this number cannot go
 * stale silently.
 *
 * Raised together with kServiceableEndpoints for the LM20 tier. Note it
 * must be raised whenever HEARTH_EP_HEAP_BYTES is: a bigger block heap
 * admits object-heavier compositions, and the maximum above is a function
 * of the block budget.
 */
#define HEARTH_OBJ_HEAP_BYTES 6528
