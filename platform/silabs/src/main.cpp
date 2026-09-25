/*
 * main.cpp - the sl_main hooks and Hearth's bootstrap task.
 *
 * Under sl_main with a kernel the SDK owns main(): main_retarget.c wraps
 * it and runs sl_main_init(), then sl_main_kernel_start(). The two hooks
 * below do NOT run at the same point, and the difference decides what may
 * go in each (SiSDK 2025.12.3,
 * platform_core/platform/service/sl_main/src/sl_main_init.c):
 *
 *   app_init_early()  called from sl_main_init():353, after
 *                     sl_clock_manager_init():319 and the device_init
 *                     steps, and BEFORE osKernelInitialize():362. The
 *                     clocks are up; the kernel does not exist yet, so
 *                     nothing here may call an API that needs a running
 *                     scheduler.
 *   app_init()        called from the SDK's own main(), src/rtos/main.c:38,
 *                     i.e. on the start task with the kernel running, after
 *                     sl_main_second_stage_init() (sl_platform_init,
 *                     sl_driver_init, sl_service_init, sl_stack_init and
 *                     sl_internal_app_init, sl_main_init.c:220-226). The last
 *                     of those is where OpenThread's instance is created, and
 *                     with it chip::Platform::MemoryInit(); see the note in
 *                     port/hearth_matter_init.cpp.
 *
 * Nothing here starts the scheduler.
 *
 * Boot contract (layout spec section 5, MG24 spec section 6): platform up,
 * console, the Matter stack, the endpoint composition, then mt_at_start() on
 * a task, which emits +MTREADY. No URC precedes the marker, so everything
 * that could raise one has to happen on the near side of it.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "cmsis_os2.h"

/* Round 2 task 2: there is no separate FreeRTOS heap in this image any more.
 * matter_platform_mg requires freertos_heap_3, the port that forwards
 * pvPortMalloc() to the C library's malloc(), which the SDK's linker wraps into
 * sl_memory_manager's .memory_manager_heap. So configTOTAL_HEAP_SIZE is gone
 * from hearth.slcp (heap_3 does not read it), kernel objects and CHIP's
 * allocations now share one pool, and xPortGetFreeHeapSize() does not exist:
 * heap_3.c does not define it, which is a LINK error rather than a wrong
 * number. sl_memory_get_free_heap_size() is the figure that replaced it, and it
 * measures the pool that now holds everything.
 */
#include "sl_memory_manager.h"

/* Round 2 task 7: the NVM3 occupancy line below. nvm3_default.h is where
 * nvm3_defaultHandle lives; nvm3_generic.h declares nvm3_getEraseCount(),
 * nvm3_getMemInfo() and the inline nvm3_countObjects(). Both are already in
 * the image through port/hearth_port_sl.c's key-value store. */
#include "nvm3.h"
#include "nvm3_default.h"

#include <app/util/endpoint-config-api.h>
#include <platform/CHIPDeviceLayer.h>

#include "hearth_console.h"
#include "hearth_log.h"
#include "hearth_matter_init.h"
#include "hearth_port.h"
#include "mt_at.h"
#include "mt_chip_events.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
#include "mt_dyn_store.h"
#include "mt_matter.h"
#include "mt_port_ids.h"

extern "C" void app_init_early(void);
extern "C" void app_init(void);

namespace {

/* The name advertised while a commissioning window is open. It REPLACES the
 * SDK's "<prefix><discriminator>" form rather than prefixing it; the note
 * beside CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX in CHIPProjectConfig.h has
 * the line references. */
constexpr const char *kBleDeviceName = HEARTH_BLE_DEVICE_NAME;

/*
 * Rebuild the endpoint composition the host declared over AT+MTEP. The
 * device does this unaided so it rejoins its fabric after a power cut
 * without waiting on the host (design spec section 5.3).
 *
 * Real since round 2 task 5; the body is platform/nrf54l15/src/main.cpp
 * 40-108 with its Zephyr log macros replaced. It is called from where it has
 * to be called from: after Server::Init (the ember tables
 * emberAfSetDynamicEndpoint() writes exist only then) and before
 * mt_at_start() (so no +MTATTR URC can precede +MTREADY).
 *
 * Order is what makes the endpoint ids reproducible: mt_devtype_create()
 * hands out 1..N in composition order from a counter reset at every boot,
 * so replaying the same stored composition yields the same ids.
 */
void rebuild_composition(void);

/* One console line of NVM3 occupancy per boot; see the definition. */
void hearth_nvm3_report(void);

/* mt_at_start() calls hearth_link_init() itself (core/mt/mt_at.c); a second
 * call here would leak the RX semaphore and TX mutex and reconfigure EUSART0
 * under a live link. */
void hearth_boot_task(void *arg)
{
    (void)arg;
    HEARTH_LOGI("boot", "boot task up, model %s", hearth_port_model());

    CHIP_ERROR err = hearth_matter_init(kBleDeviceName);
    if (err != CHIP_NO_ERROR) {
        /* Nothing to fall back to: an image whose stack did not come up
         * cannot serve the AT+MT contract, and answering +MTREADY would be a
         * lie. Say which error on the console and stop. fw/flash.py's exit
         * status is the other half of this: no +MTREADY is a failed flash. */
        HEARTH_LOGE("boot", "matter init failed: %" CHIP_ERROR_FORMAT, err.Format());
        vTaskDelete(NULL);
        return;
    }

    {
        /* This runs on the boot task, not the CHIP event loop, so every call
         * into the stack from here needs the lock (SL_MATTER_STACK_LOCK_-
         * TRACKING_MODE is FATAL in config/sl_matter_config.h: a violation
         * kills the device rather than racing quietly).
         *
         * The catalogue endpoint exists only to compile cluster server code
         * into the image (MG24 spec section 3); it is never visible on the
         * fabric, and this is where it stops being. */
        chip::DeviceLayer::StackLock lock;
        if (!emberAfEndpointEnableDisable(kCatalogueEndpointId, false)) {
            /* It returns false only for an endpoint index it cannot find
             * (attribute-storage.cpp), which would mean the generated
             * FIXED_ENDPOINT_ARRAY and mt_port_ids.h have drifted apart. Not
             * fatal: the endpoint would simply stay visible on the fabric, and
             * a device that still answers the AT contract is more use than one
             * that stops. Say so, loudly. */
            HEARTH_LOGE("boot", "catalogue endpoint %u not disabled: no such endpoint",
                        (unsigned)kCatalogueEndpointId);
        }

        /* The +MTEVT sources, under the same lock and for the same reason:
         * AddEventHandler, SetAppDelegate and AddFabricDelegate all touch
         * stack-owned state from a task that is not the CHIP event loop. It
         * has to be after Server::Init (the window manager and the fabric
         * table exist only then) and before mt_at_start(), so the window the
         * server opened during its own init is already being tracked when the
         * marker goes out. Nothing the delegates emit can precede +MTREADY:
         * mt_at_event() goes through mt_at_urc()'s s_at_up guard. */
        CHIP_ERROR evt_err = mt_chip_events_register();
        if (evt_err != CHIP_NO_ERROR) {
            /* Not fatal, unlike a failed stack init: the AT surface still
             * answers every command, the host simply has to poll AT+MTSTATE?
             * instead of being told. Say which error, loudly. */
            HEARTH_LOGE("boot", "event registration failed: %" CHIP_ERROR_FORMAT, evt_err.Format());
        }
    }

    rebuild_composition();
    mt_at_start();          /* emits +MTREADY once the parser task is up */
    /* The boot replay of +MTEVT:0, after the marker and with no lock held:
     * it reads mt_matter_state(), which takes the stack lock itself. */
    mt_chip_events_after_ready();
    HEARTH_LOGI("boot", "+MTREADY sent, free heap %u B",
                (unsigned)sl_memory_get_free_heap_size());
    hearth_nvm3_report();
    vTaskDelete(NULL);
}

/*
 * One line of NVM3 occupancy, once per boot, after the marker.
 *
 * Round 2 task 7 added it because the design spec's section 6 asks for NVM3
 * occupancy after commissioning and there is no other way to read it on this
 * bench: the part's debug lock is open but Commander drives J-Link probes
 * only, so an SWD read of the region is not available here, and the AT surface
 * has no command for it. A boot line is enough, because the figure that
 * matters is the one at the boot AFTER a commissioning; it costs one enumerate
 * pass over the objects the driver has already cached.
 *
 * What the three numbers are:
 *   objects      valid objects in the instance (nvm3_countObjects(), which is
 *                nvm3_enumObjects() over the whole key range)
 *   free         availableMemory from nvm3_getMemInfo(): usable bytes before a
 *                repack is triggered, NOT the erased-byte count. NVM3 is
 *                log-structured, so a deleted or rewritten object keeps
 *                occupying flash until the repack collects it, and this figure
 *                already accounts for that.
 *   erase        the highest erase count over the instance's pages
 *                (nvm3_getEraseCount()), the flash-wear figure.
 *
 * The region itself is fixed at build time: NVM3_DEFAULT_NVM_SIZE 40960 in
 * hearth.slcp, placed by the generated linker file at __nvm3Base
 * (linker_nvm_end - SIZEOF(.nvm)), i.e. the top of the application region. The
 * section is a DSECT, so it costs the image nothing and a .gbl upload that
 * stops short of it leaves the instance intact. The base and the size are read
 * off the live handle rather than from the config macro, so the line reports
 * where the instance ACTUALLY is and a linker change cannot make it lie.
 */
void hearth_nvm3_report(void)
{
    nvm3_MemInfo_t info = { false, 0, false, 0 };
    uint32_t erase = 0;
    size_t objects = nvm3_countObjects(nvm3_defaultHandle);
    sl_status_t mem_rc = nvm3_getMemInfo(nvm3_defaultHandle, &info);
    sl_status_t era_rc = nvm3_getEraseCount(nvm3_defaultHandle, &erase);

    if (mem_rc != SL_STATUS_OK || era_rc != SL_STATUS_OK) {
        /* Say which call failed rather than printing a zero that reads like a
         * measurement. Not fatal: nothing in the boot contract depends on it. */
        HEARTH_LOGE("boot", "nvm3: %u object(s), meminfo rc 0x%08lX, erasecount rc 0x%08lX",
                    (unsigned)objects, (unsigned long)mem_rc, (unsigned long)era_rc);
        return;
    }
    HEARTH_LOGI("boot", "nvm3: %u object(s), %u B free of %u B at 0x%08lX, erase count %lu",
                (unsigned)objects, (unsigned)info.availableMemory,
                (unsigned)nvm3_defaultHandle->nvmSize,
                (unsigned long)(uintptr_t)nvm3_defaultHandle->nvmAdr, (unsigned long)erase);
}

void rebuild_composition(void)
{
    /*
     * Run-once, made checkable here rather than left as a property of the
     * single call site. mt_devtype_create() allocates each endpoint's block
     * from a bump arena and never frees (port/mt_devtypes_sl.cpp explains why
     * that is safe), and the whole argument rests on the allocation sequence
     * being one monotonic run in composition order. A second call would stack
     * a second set of blocks on the first and quietly halve the capacity the
     * README documents, so say so locally instead of making a future reader
     * trace callers to be sure.
     */
    static bool s_rebuilt;
    if (s_rebuilt) {
        HEARTH_LOGE("boot", "rebuild_composition() called twice; endpoint blocks are "
                            "allocate-only");
        return;
    }
    s_rebuilt = true;

    mt_composition_t comp;
    int rc = mt_comp_store_load(&comp);
    if (rc < 0) {
        HEARTH_LOGE("boot", "composition load failed, starting unconfigured");
        comp.count = 0;
    } else if (rc == 1) {
        HEARTH_LOGI("boot", "no stored composition, starting unconfigured");
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
                HEARTH_LOGE("boot", "endpoint %u parent index %u not live, aborting rebuild",
                            (unsigned)i, (unsigned)comp.parent[i]);
                break;
            }
            parent_devtype = dt;
            parent_ep_id = eid;
        }
        if (mt_devtype_create(comp.devtype[i], comp.variant[i], parent_devtype, parent_ep_id,
                              &ep_id) != 0) {
            /*
             * Abort the whole rebuild rather than skipping the failed entry.
             * A failed create consumes no endpoint id, so every endpoint
             * after it would shift down by one and a commissioned device
             * would silently get the wrong data model. See design spec 12.1.
             * The same abort covers a parenting failure (mt_devtype_create()
             * returns -1 for that too) and, on this image, a device type the
             * catalogue knows but this build does not yet construct: a
             * live endpoint with the wrong parent, and a missing endpoint
             * followed by renumbered ones, are the identical kind of
             * silently-wrong data model.
             */
            HEARTH_LOGE("boot", "endpoint %u (0x%04X) failed, aborting rebuild", (unsigned)i,
                        (unsigned)comp.devtype[i]);
            break;
        }
        mt_matter_record_endpoint(comp.devtype[i], ep_id, comp.variant[i], comp.parent[i]);
    }

    HEARTH_LOGI("boot", "composition rebuilt: %u endpoint(s)",
                (unsigned)mt_matter_endpoint_count());
    /* Beside the line it explains: how much of the endpoint arena that
     * composition actually cost. */
    mt_dyn_arena_report();
    mt_obj_arena_report();
}

} // namespace

/* Runs before the kernel starts: the clocks are up (sl_clock_manager_init()
 * and the device_init steps ran earlier in sl_main_init), so the console can
 * exist before anything else logs. */
extern "C" void app_init_early(void)
{
    hearth_console_init();
    /* After the console and before everything else: the SDK's Bluetooth and
     * OpenThread tasks log through CHIP from the moment the kernel starts, and
     * those lines are the ones a boot-order question needs. */
    hearth_matter_log_route_init();
    HEARTH_LOGI("boot", "Hearth on USART0 TX PA00, 115200 8N1 (sl_main)");
}

/* Runs on the sl_main start task with the kernel running. */
extern "C" void app_init(void)
{
    /* Stack: 1,280 words is 5,120 bytes, the size of the sample's own
     * bootstrap thread (kMainTaskStackSize, MatterConfig.cpp:192, where
     * osThreadAttr_t's stack_size is in BYTES and xTaskCreate's depth is in
     * WORDS). It was 1,024 words through round 1 and round 2 tasks 1 and 2,
     * when the task's deepest call was mt_at_start(); it now runs the whole of
     * hearth_matter_init(), which is the path the sample sized that thread for.
     *
     * PRIORITY IS LOAD-BEARING, and it is the sample's for the same reason its
     * stack is. kMainTaskAttr (MatterConfig.cpp:194-200) asks for
     * osPriorityRealtime7, which the CMSIS-RTOS2 shim turns into FreeRTOS
     * priority (prio - 1), i.e. 54 of configMAX_PRIORITIES 56.
     *
     * Through round 2 task 3 this task ran at tskIDLE_PRIORITY + 1, and the
     * Matter stack then came up but never advertised over BLE. The cause,
     * measured on the console once the CHIP log route was installed early
     * enough to see it:
     *
     *   I chip: [DL] Bluetooth stack booted: v11.0.2-b0
     *   E chip: [DL] Failed to schedule work: 1c
     *   ... later ...
     *   I chip: [DL] Init CHIP Stack
     *
     * sl_main creates the start task at osPriorityRealtime7 as well
     * (sl_main_kernel.c:74-88, "the highest priority") and
     * SL_MAIN_ENABLE_START_TASK_PRIORITY_CHANGE is 0, so it keeps that
     * priority through app_init(). A boot task below the Bluetooth event
     * handler task (SL_BT_RTOS_EVENT_HANDLER_TASK_PRIORITY 50, FreeRTOS 49)
     * therefore runs AFTER the Bluetooth stack's boot event is dispatched.
     * BLEManagerImpl::HandleBootEvent() (BLEManagerImpl.cpp:785-789) sets
     * Flags::kSiLabsBLEStackInitialize and its ScheduleWork fails because the
     * platform manager does not exist yet; then _Init() runs inside
     * InitChipStack() and its mFlags.ClearAll() (:155) throws the flag away.
     * No second boot event ever comes, so DriveBLEState() returns at its first
     * line (:417) for the life of the image and CHIPoBLE never advertises.
     *
     * At the sample's priority the whole bring-up completes before the
     * Bluetooth event handler is scheduled, which is what makes the flag
     * arrive after _Init rather than before it. The cost is that the console's
     * blocking writes (about 10 ms a line at 115200) hold the CPU against
     * every lower task for the ~350 ms the init log takes; that is a boot-time
     * cost on a task that deletes itself a few instructions after +MTREADY. */
    if (xTaskCreate(hearth_boot_task, "hearth_boot", 1280, NULL,
                    (UBaseType_t)(osPriorityRealtime7 - 1), NULL) != pdPASS) {
        /* Round 1 deferral: a boot task that fails to create was silent on
         * both UARTs. Say so on the console; there is nothing else to do. */
        HEARTH_LOGE("boot", "boot task could not be created (heap %u B free)",
                    (unsigned)sl_memory_get_free_heap_size());
    }
}
