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
 * be a comment that lies. The MG24 port's own allocation strategy belongs to
 * the task that builds the dynamic endpoints, and that task adds its budget
 * here with its own arithmetic beside it. The nRF file is the reference for
 * how that arithmetic is written down.
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
