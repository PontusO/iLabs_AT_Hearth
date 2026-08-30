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
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
#include "mt_matter.h"
}
#include "mt_port_ids.h"

LOG_MODULE_REGISTER(hearth_main, LOG_LEVEL_INF);

/*
 * Rebuild the endpoint composition the host declared over AT+MTEP. The
 * device does this unaided so it rejoins its fabric after a power cut
 * without waiting on the host (design spec section 5.3).
 *
 * Order is what makes the endpoint ids reproducible: mt_devtype_create()
 * hands out 1..N in composition order from a counter reset at every boot,
 * so replaying the same stored composition yields the same ids.
 */
static void rebuild_composition(void)
{
    /*
     * Run-once, made checkable here rather than left as a property of the
     * single call site. mt_devtype_create() allocates each endpoint's block
     * from a dedicated heap and never frees (port/mt_devtypes_zephyr.cpp
     * explains why that is safe), and the whole argument rests on the
     * allocation sequence being one monotonic run in composition order. A
     * second call would stack a second set of blocks on the first and
     * quietly halve the capacity the README documents, so say so locally
     * instead of making a future reader trace callers to be sure.
     */
    static bool s_rebuilt;
    if (s_rebuilt) {
        LOG_ERR("rebuild_composition() called twice; endpoint blocks are allocate-only");
        return;
    }
    s_rebuilt = true;

    mt_composition_t comp;
    int rc = mt_comp_store_load(&comp);
    if (rc < 0) {
        LOG_ERR("composition load failed, starting unconfigured");
        comp.count = 0;
    } else if (rc == 1) {
        LOG_INF("no stored composition, starting unconfigured");
        comp.count = 0;
    }

    for (uint16_t i = 0; i < comp.count; i++) {
        uint16_t ep_id = 0;
        uint32_t parent_devtype = 0;
        uint16_t parent_ep_id = 0;
        uint32_t dt = 0;
        uint16_t eid = 0;
        uint8_t var = 0;
        uint8_t pidx = 0;
        if (comp.parent[i] != MT_COMP_NO_PARENT) {
            if (mt_matter_endpoint_info(comp.parent[i], &dt, &eid, &var, &pidx) != 0) {
                /* The composition says this endpoint has a parent and the
                 * parent is not live. Creating it unparented would present
                 * the fabric a different device than the stored composition
                 * describes: the same silently-wrong data model the abort
                 * below exists to prevent. */
                LOG_ERR("endpoint %u parent index %u not live, aborting rebuild", i,
                        (unsigned)comp.parent[i]);
                break;
            }
            parent_devtype = dt;
            parent_ep_id = eid;
        }
        if (mt_devtype_create(comp.devtype[i], comp.variant[i], parent_devtype, parent_ep_id,
                              &ep_id) != 0) {
            /*
             * Abort the whole rebuild rather than skipping the failed
             * entry. A failed create consumes no endpoint id, so every
             * endpoint after it would shift down by one and a commissioned
             * device would silently get the wrong data model. See design
             * spec 12.1. The same abort covers a parenting failure
             * (mt_devtype_create() returns -1 for that too): a live
             * endpoint with the wrong parent is the identical kind of
             * silently-wrong data model.
             */
            LOG_ERR("endpoint %u (0x%04X) failed, aborting rebuild", i, (unsigned)comp.devtype[i]);
            break;
        }
        mt_matter_record_endpoint(comp.devtype[i], ep_id, comp.variant[i], comp.parent[i]);
    }

    LOG_INF("composition rebuilt: %u endpoint(s)", mt_matter_endpoint_count());
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

    /* Catalogue batch 7a: construct and register the MeterIdentification
     * Instances for every live meter endpoint. Nothing in the SDK ever
     * calls their Init() (the mt_meter_register_all() doc comment in
     * mt_matter_zephyr.cpp), and it cannot run inside
     * mt_devtype_create()'s own second halves by the C6's precedent
     * shape, so the scan runs here: after the rebuild (the clusters
     * exist), before mt_at_start() (every meter endpoint answers before
     * +MTREADY lets the host ask). The server is already running at this
     * point on this platform; the scan takes the stack lock internally,
     * the same timing every create-path Init() above already has. */
    mt_meter_register_all();

    mt_at_start();

    while (true) {
        Nrf::DispatchNextTask();
    }
    return 0;
}
