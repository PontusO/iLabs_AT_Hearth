/*
 * main.cpp - nRF54L15 boot path. Boot contract per the layout spec
 * section 5 and the Matter core spec section 4: CHIP server up first,
 * dynamic endpoints rebuilt from the stored composition, then
 * mt_at_start(), which emits +MTREADY. No URC precedes the boot marker.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/matter_init.h>
#include <app/task_executor.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <platform/PlatformManager.h>

extern "C" {
#include "mt_at.h"
}

LOG_MODULE_REGISTER(hearth_main, LOG_LEVEL_INF);

/* The catalogue endpoint exists only to compile cluster server code
 * into the image (spec section 3); it is never visible on the fabric. */
static constexpr chip::EndpointId kCatalogueEndpointId = 240;

/* Filled in by the dynamic-endpoint task: reads the stored composition
 * and creates one dynamic endpoint per entry. Until then the device is
 * commissionable but bare. */
static void rebuild_composition(void)
{
}

int main(void)
{
    CHIP_ERROR err = Nrf::Matter::PrepareServer();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("PrepareServer failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }
    err = Nrf::Matter::StartServer();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("StartServer failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }

    {
        chip::DeviceLayer::StackLock lock;
        emberAfEndpointEnableDisable(kCatalogueEndpointId, false);
    }

    rebuild_composition();

    mt_at_start();

    while (true) {
        Nrf::DispatchNextTask();
    }
    return 0;
}
