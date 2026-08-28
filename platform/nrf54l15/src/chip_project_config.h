/* chip_project_config.h - project-level CHIP configuration.
 *
 * 28 mirrors MT_COMP_MAX_ENDPOINTS (core/include/mt_composition.h): the
 * dynamic endpoint table is exactly as deep as the composition the AT
 * contract can stage. mt_devtypes_zephyr.cpp static_asserts the two
 * stay equal; this header cannot include core headers because CHIP
 * includes it everywhere.
 */
#pragma once

#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 28
