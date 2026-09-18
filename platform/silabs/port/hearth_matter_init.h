/*
 * hearth_matter_init.h - Hearth's Matter stack bring-up on the MGM240PA32VNA3.
 *
 * C++ linkage on purpose: the only caller is src/main.cpp's boot task and the
 * return type is CHIP_ERROR. Nothing in core/ sees this header.
 */

#pragma once

#include <lib/core/CHIPError.h>

/*
 * Bring the Matter stack all the way up, in the order the Silicon Labs sample
 * uses (third_party/matter_sdk/examples/platform/silabs/MatterConfig.cpp,
 * SilabsMatterConfig::InitMatter) minus everything that belongs to the sample's
 * application layer.
 *
 * ble_name is the name advertised while a commissioning window is open. It is
 * passed to ConnectivityMgr().SetBLEDeviceName(), which REPLACES the SDK's
 * "<prefix><discriminator>" form rather than decorating it; see the note beside
 * CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX in src/CHIPProjectConfig.h.
 *
 * Postcondition on CHIP_NO_ERROR: the CHIP platform layer is initialised, the
 * provisioning storage serves the device instance info, the commissionable data
 * and the device attestation credentials, OpenThread is initialised as a router
 * with its task running, chip::Server is initialised over the generated
 * (codegen/ember) data model, and the CHIP event loop task is running. The
 * caller may take chip::DeviceLayer::StackLock and touch the ember tables from
 * that moment on, and must do so for every call into the stack that does not
 * already run on the CHIP task.
 *
 * On any other return value nothing is guaranteed to be running and the caller
 * must not touch the stack. The function does not roll back: a failure here is
 * a boot failure.
 *
 * Call once, from a task, with the kernel running. It must not be called from
 * app_init_early(): it allocates, takes mutexes and creates tasks.
 */
CHIP_ERROR hearth_matter_init(const char *ble_name);
