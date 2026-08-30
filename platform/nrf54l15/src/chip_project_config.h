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

/*
 * How many endpoints may carry ElectricalEnergyMeasurement AT ONCE.
 *
 * REQUIRES THE SDK PATCH in ../sdk-patches. Stock connectedhomeip does not
 * read this macro: it sizes its measurement table by the whole dynamic
 * endpoint space instead, 17 x 496 = 8,432 bytes charged the moment the
 * cluster enters the build, whether or not any composition declares an
 * energy endpoint. CMakeLists.txt refuses to configure against an unpatched
 * tree precisely so that this line cannot become a no-op nobody notices;
 * see sdk-patches/README.md.
 *
 * 4 is the MT_DEM_MAX / MT_WHM_MAX depth (core/include/mt_matter.h), the
 * shape of pool every energy family in this port already uses, and it is
 * comfortably above the one or two EEM-bearing endpoints any plausible
 * composition declares. A fifth simultaneous EEM endpoint does not fail the
 * composition: MeasurementDataForEndpoint() answers nullptr, which the
 * cluster's own readers already render as null or NOT_FOUND, and the SDK
 * logs the macro to raise. A slot is reclaimed automatically once its
 * endpoint stops serving the cluster, so this is a concurrency bound and
 * not a lifetime budget.
 */
#define CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES 4
