/* CHIPProjectConfig.h - project-level CHIP configuration for Hearth on the
 * MGM240PA32VNA3.
 *
 * CHIP finds this file by NAME, not by path: the extension's
 * slc/inc/system/SystemBuildConfig.h defines
 * CHIP_PROJECT_CONFIG_INCLUDE as <CHIPProjectConfig.h> and
 * src/lib/core/CHIPConfig.h includes that. hearth.slcp puts this directory on
 * the include path, which is the same way the stock lighting-app's is found.
 *
 * It therefore reaches EVERY translation unit in the build, including C ones,
 * so it can include no header of this project's own. Every value below is a
 * literal, and the mirrors are asserted where the real definition lives
 * (mt_devtypes_sl.cpp, when the port arrives) rather than trusted.
 *
 * Deliberately NOT here: OTA (this product's update story is host-driven
 * serial flashing, FIRMWARE_UPDATE_SPEC.md, and the OTA Requestor and Provider
 * clusters are disabled in data_model/hearth.zap), the shell, and the LCD and
 * button settings the sample carries for a development kit this carrier is not.
 */
#pragma once

/* How many endpoints this build can stand up and SERVE at once.
 *
 * 16 mirrors kServiceableEndpoints (port/mt_port_ids.h on the nRF arm, and the
 * MG24 port when it lands). It is deliberately NOT MT_COMP_MAX_ENDPOINTS
 * (core/include/mt_composition.h, 28), which is how many a host may DECLARE
 * over AT+MTEP: acceptance is a wire-contract number every platform shares,
 * capacity is what a given part affords. The dynamic endpoint table is exactly
 * as deep as the tier serves.
 *
 * This value sizes every compile-time per-endpoint pool inside CHIP, which is
 * most of what a reduction here buys back.
 */
#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 16

/* How many endpoints may carry ElectricalEnergyMeasurement AT ONCE.
 *
 * REQUIRES THE SDK PATCH in ../sdk-patches. Stock connectedhomeip does not read
 * this macro: it sizes its measurement table by the whole dynamic endpoint
 * space instead, 17 x 496 = 8,432 bytes charged the moment the cluster enters
 * the build, whether or not any composition declares an energy endpoint.
 * toolchain.env refuses to finish against an unpatched or wrongly-patched tree
 * precisely so that this line cannot become a no-op nobody notices; see
 * ../sdk-patches/README.md.
 *
 * 8 is MT_MEAS_MAX (core/include/mt_matter.h), this port's own answer to how
 * many measurement-capable endpoints one composition may carry. A composition
 * this firmware ACCEPTS must be one it can SERVE, so the pool is sized from the
 * port's own capacity rather than from a plausible number.
 *
 * The cluster is not in this round's build at all; the macro is here because it
 * belongs with the patch and the gate, which are.
 */
#define CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES 8

/* Identity. Development credentials: 0xFFF1 is the CSA test vendor id, and this
 * firmware is uncertified (README, and the naming note in the firmware repo's
 * CLAUDE.md). A consumer hub is expected to refuse it.
 *
 * 0x8010 is Hearth's product id within the test vendor space, distinct from the
 * SDK samples' (the lighting-app uses 0x8005) so that a bench with both running
 * can tell them apart.
 */
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID 0xFFF1
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID 0x8010
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "iLabs"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Hearth"

/* The name the device advertises over BLE while a commissioning window is open.
 *
 * There are TWO of these and only one of them is ever on the air.
 *
 * HEARTH_BLE_DEVICE_NAME is what src/main.cpp hands hearth_matter_init(), which
 * passes it to ConnectivityMgr().SetBLEDeviceName(). That call sets
 * Flags::kDeviceNameSet (BLEManagerImpl.cpp:251-266) and the advertisement then
 * carries this string verbatim. It is the equivalent of the sample's
 * BLE_DEV_NAME ("SL-Light", examples/lighting-app/silabs/include/AppConfig.h:35),
 * which is why the stock example advertises as SL-Light and not as
 * MATTER-<discriminator>.
 *
 * CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX is the SDK's fallback, used to build
 * "<prefix><4-digit discriminator>" only when no name has been set
 * (BLEManagerImpl.cpp:466-471). Round 2 task 2 set it because the SDK default is
 * "MATTER-", which would put the word on the air as this product's name: the
 * protocol is named, the product is Hearth. Round 2 task 3 made it inert by
 * setting a name, and it is kept because it is still the right answer for the
 * path that does not.
 *
 * Which of the two a shipping product should advertise is a wire-surface
 * question for the qualification round. A fixed name is easier for a host to
 * recognise; the discriminator form tells two units apart, which on this build
 * it would not, because the discriminator is the fixed test value below.
 */
#define HEARTH_BLE_DEVICE_NAME "Hearth"
#define CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX "HEARTH-"

/* The CHIP event loop task's stack, in bytes (PlatformManagerImpl_FreeRTOS).
 *
 * 8,192 is the SDK's own default for this platform and is written down rather
 * than inherited: CHIPDevicePlatformConfig.h:133-146 records that a Thread
 * lighting app's high stack watermark during commissioning is 5,232 bytes, and
 * 6,576 when built with LTO. The sample's own CHIPProjectConfig.h sets 10,240
 * only under __ZEPHYR__, so 8,192 is the value the shipping Silicon Labs
 * examples actually run on.
 *
 * Hearth puts LESS on this task than the sample does, not more: it has no
 * AppTask, and the AT command families run on the AT parser task and enter the
 * stack under the lock. The number is here so that a later round that moves
 * work onto the event loop changes it deliberately.
 */
#define CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE (8 * 1024)

/* Transport. BLE for commissioning, Thread for operation, and a router-capable
 * Full Thread Device: the role is a product capability and is stated here
 * rather than inherited from a default that could move (DE412). FTD is also
 * what the matter_thread component requires without matter_icd_core.
 */
#define CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE 1
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD 1
#define CHIP_DEVICE_CONFIG_THREAD_FTD 1

/* Onboarding. The SDK test values, the same pair the samples use and the same
 * pair the bench's chip-tool invocation passes. They are defaults for a device
 * with no provisioned credentials, which is every device this round builds.
 */
#ifndef CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE
#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE 20202021
#endif
#ifndef CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR
#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR 0xF00
#endif
#define CHIP_DEVICE_CONFIG_TEST_SERIAL_NUMBER "TEST_SN"

/* Security test mode circumvents message encryption and authentication. The
 * sample sets it to 0 explicitly and so does this: a macro that dangerous is
 * worth seeing written down as off.
 */
#define CHIP_CONFIG_SECURITY_TEST_MODE 0

/* Event logging, carried from the sample unchanged: UTC timestamps on events,
 * and the debug event buffer at the sample's size.
 */
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS 1
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_DEBUG_BUFFER_SIZE (512)

/* Active-mode retransmit interval, carried from the sample. This is the value
 * a peer learns through the CRA key in the operational TXT record, so it is
 * part of the wire behaviour rather than a local tuning knob.
 */
#define CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL (2000_ms32)
