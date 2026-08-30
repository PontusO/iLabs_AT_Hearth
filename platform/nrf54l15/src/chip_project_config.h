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
 * 8 is MT_MEAS_MAX (core/include/mt_matter.h), this port's own answer to
 * "how many measurement-capable endpoints may one composition carry", and
 * it is the only defensible number here. It was 4 for one round, chosen as
 * comfortably above any plausible composition, and that was an assertion
 * about plausibility rather than a bound: this port permits 16 dynamic
 * endpoints, six catalogue device types carry the cluster, and nothing
 * counted them. A composition this firmware ACCEPTS must be one it can
 * SERVE, so the pool is sized from the port's own capacity, and
 * mt_matter_zephyr.cpp static_asserts that the two cannot drift apart.
 *
 * Exhaustion is loud rather than silent: mt_matter_eem_reserve() claims a
 * slot before the endpoint is built and stops the composition rebuild
 * naming this macro, the way every other pool in this port does. Without
 * that gate a ninth EEM endpoint would apply cleanly and then answer
 * Failure to a controller reading its MANDATORY Accuracy attribute, with
 * nothing on the AT wire to say so. The gate is unreachable while every
 * EEM-bearing device type also declares ElectricalPowerMeasurement, which
 * exhausts the MT_MEAS_MAX EPM pool first; it is there for the seventh type
 * that does not.
 *
 * A slot is reclaimed automatically once its endpoint stops serving the
 * cluster, so this is a concurrency bound and not a lifetime budget.
 */
#define CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES 8
