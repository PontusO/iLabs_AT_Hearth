/* chip_project_config.h - project-level CHIP configuration.
 *
 * 16 mirrors kServiceableEndpoints (port/mt_port_ids.h), the number of
 * endpoints this build can stand up and SERVE at once. It is deliberately
 * NOT MT_COMP_MAX_ENDPOINTS (core/include/mt_composition.h, 28), which is
 * how many a host may DECLARE over AT+MTEP: acceptance is a wire-contract
 * number both platforms share, capacity is what this 256 KB part affords.
 * See the long note on kServiceableEndpoints for the split, and the
 * README's "Endpoint capacity" section for what a host sees when a stored
 * composition is longer than capacity.
 *
 * This value sizes every compile-time per-endpoint pool inside CHIP, which
 * is most of what the reduction buys back: ColorControlServer's eleven
 * transition and quiet-reporting arrays, level-control's state table,
 * BooleanState's server pool, and the Thermostat / WindowCovering /
 * FanControl delegate tables.
 *
 * The literal is duplicated here rather than included because CHIP pulls
 * this header in everywhere, including C translation units, so it cannot
 * include the port's C++ headers. mt_devtypes_zephyr.cpp static_asserts
 * that the mirror has not drifted.
 */
#pragma once

#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 16
