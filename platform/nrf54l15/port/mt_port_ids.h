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
