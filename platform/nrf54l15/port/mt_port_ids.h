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
 *     alone carries eleven arrays sized fixed + dynamic count
 *     (color-control-server.h:293-323); level-control's state table,
 *     BooleanState's server pool and the Thermostat / WindowCovering /
 *     FanControl delegate tables are all sized the same way.
 *
 * 16 rather than 28 is a deliberate trade for an nRF54L15's 256 KB. A host
 * may still DECLARE up to 28 endpoints and the composition still persists
 * intact; a composition longer than 16 simply fails its rebuild at the
 * seventeenth endpoint, loudly, and leaves the device bare rather than
 * half-built (design spec 12.1). The nRF54LM20 tier (512 KB, supported
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
