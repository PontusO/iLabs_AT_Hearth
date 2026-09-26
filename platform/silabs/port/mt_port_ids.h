/*
 * mt_port_ids.h - numeric ids shared between the MG24 port's own translation
 * units, so a value like the catalogue endpoint id is named once instead of
 * risking two independently-typed literals drifting apart.
 *
 * This is platform/nrf54l15/port/mt_port_ids.h with the two Zephyr heap
 * budgets (HEARTH_EP_HEAP_BYTES and HEARTH_OBJ_HEAP_BYTES) left out. They
 * size K_HEAP_DEFINE arenas in mt_devtypes_zephyr.cpp and mt_matter_zephyr.cpp
 * and their whole rationale is stated against a Zephyr allocator that does not
 * exist here; carrying them over as numbers with a foreign explanation would
 * be a comment that lies. Round 2 task 5 is the task that builds the dynamic
 * endpoints, and HEARTH_EP_ARENA_BYTES below is its budget, with its own
 * arithmetic beside it. There is still no object-arena budget here, because
 * this round builds no device type whose cluster server needs a per-endpoint
 * delegate; see the note beside the live endpoint table in
 * port/mt_matter_sl.cpp.
 */

#pragma once

#include <stdint.h>

/*
 * The catalogue endpoint exists only to compile cluster server code into the
 * image (MG24 spec section 3); it is never visible on the fabric.
 * src/main.cpp disables it in the boot task once Server::Init has built the
 * ember tables (emberAfEndpointEnableDisable, <app/util/endpoint-config-api.h>),
 * and the same id is why the upward port's attribute-change callback will skip
 * it, so a write to catalogue-endpoint storage never turns into a +MTATTR URC
 * for an endpoint the host was never told exists.
 *
 * 240 is 0x00F0, the second entry of FIXED_ENDPOINT_ARRAY in
 * data_model/zap-generated/endpoint_config.h.
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
 * serve concurrently. It sizes every compile-time per-endpoint pool inside
 * CHIP through CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT, which
 * src/CHIPProjectConfig.h mirrors from this value, as well as the port's own
 * dynamic endpoint table when that arrives.
 *
 * 16 is the nRF54L15 arm's number and is kept here so the two Thread ports
 * answer the same. A host may still DECLARE up to 28 endpoints and the
 * composition still persists intact; a composition longer than 16 fails its
 * rebuild at the seventeenth endpoint, loudly. That abort is stop-at-failure,
 * not roll-back (AT_MT_SPEC.md 501-506), so the first sixteen stay live as a
 * prefix with unchanged ids and the rest are absent.
 *
 * The port's device-type translation unit static_asserts both halves of the
 * split when it lands: that CHIPProjectConfig.h still mirrors this number, and
 * that capacity never exceeds acceptance.
 */
constexpr uint16_t kServiceableEndpoints = 16;

/*
 * The endpoint block arena, round 2 task 5.
 *
 * One block per created dynamic endpoint, holding that endpoint's
 * DataVersion array and its attribute slots (port/mt_devtypes_sl.cpp has the
 * layout, the per-type sizing table and the compiler-checked floor). The
 * arena is a static array with a bump allocator, not a heap: this image has
 * exactly one heap (sl_memory_manager's, shared by FreeRTOS, CHIP, mbedTLS
 * and OpenThread since freertos_heap_3 arrived with the Matter platform in
 * task 2), and an endpoint composition must not be able to starve it. The
 * cost model is therefore rounding alone, with no allocator overhead at all:
 * a block costs its payload rounded up to 8, and the arena's usable bytes
 * are its gross bytes. The nRF's equivalent numbers are not comparable and
 * were deliberately not copied: they price Zephyr's sys_heap chunk header
 * and bucket table.
 *
 * 9,600 is 16 x 600, i.e. kServiceableEndpoints blocks of the widest device
 * type this build declares (the extended colour light: 5 clusters and 36
 * attribute slots, 596 payload bytes, 600 after rounding to 8). Catalogue
 * batch 2 moved the widest from batch 1's dimmable light and dimmable
 * plug-in unit (which share a cluster list: 4 clusters, 20 slots, 336
 * payload, and 336 is already a multiple of 8): 536 for the colour
 * temperature light, 256 for the thermostat, 224 for the window covering,
 * 192 for the on/off light and the on/off plug-in unit, 176 for the fan,
 * 160 for the temperature, humidity, pressure, light, flow and occupancy
 * sensors, 128 for the air quality sensor and the four boolean-state
 * sensors. So every composition this image accepts, it can build, which is
 * a stronger promise than the nRF arm makes (its 8,112 usable bytes hold
 * thirteen of the same extended colour light, not sixteen).
 *
 * Catalogue batch 1 kept that promise by raising this number from 3,072
 * rather than lowering the floor to the nRF's eight: the ruling of
 * 2026-09-22, made with the arithmetic in front of it, +2,304 B of .bss for
 * a capacity claim that needs no footnote. Catalogue batch 2 kept it on the
 * same terms: +4,224 B of .bss, raised to 9,600 rather than lowered to the
 * nRF's floor, the ruling of 2026-09-24. The floor assertion in
 * mt_devtypes_sl.cpp is written to FAIL when a wider device type is added,
 * so the batch that adds one has to choose between raising this number and
 * lowering the promise, in the open.
 */
#define HEARTH_EP_ARENA_BYTES 9600

/*
 * The cluster-object arena.
 *
 * The second half of the nRF's two-heap model, in this port's bump-arena
 * form (port/mt_dyn_store.h owns the cost story for both halves).
 *
 * DE555 (2026-09-26): sixteen of the largest per-endpoint object, the
 * OperationalState Delegate + Instance pair (256 B on the nRF's
 * measurement; its exact size here is pinned by a static_assert beside
 * HearthOpStateDelegate in mt_matter_sl_b4.inc); the valve delegate
 * (8 B) and the chime pair (56 B) fit within;
 * +3,968 B of .bss over batch 3.
 *
 * DE541: batch 3's minimal form, 128 B of valve delegates only.
 */
#define HEARTH_OBJ_ARENA_BYTES 4096
