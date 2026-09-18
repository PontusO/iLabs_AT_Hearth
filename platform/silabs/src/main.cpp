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

#include <app/util/endpoint-config-api.h>
#include <platform/CHIPDeviceLayer.h>

#include "hearth_console.h"
#include "hearth_log.h"
#include "hearth_matter_init.h"
#include "hearth_port.h"
#include "mt_at.h"
#include "mt_port_ids.h"

extern "C" void app_init_early(void);
extern "C" void app_init(void);

namespace {

/* The name advertised while a commissioning window is open. It REPLACES the
 * SDK's "<prefix><discriminator>" form rather than prefixing it; the note
 * beside CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX in CHIPProjectConfig.h has
 * the line references. */
constexpr const char *kBleDeviceName = HEARTH_BLE_DEVICE_NAME;

/* Rebuild the endpoints the host declared over AT+MTEP. Empty until the
 * upward port lands: it needs the dynamic endpoint machinery and the device
 * type catalogue, neither of which exists yet. It is called from where it has
 * to be called from, which is why the empty version is here rather than
 * nowhere: after Server::Init (the ember tables exist only then) and before
 * mt_at_start() (so no +MTATTR URC can precede +MTREADY). */
void rebuild_composition(void);

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
        emberAfEndpointEnableDisable(kCatalogueEndpointId, false);
    }

    rebuild_composition();
    mt_at_start();          /* emits +MTREADY once the parser task is up */
    HEARTH_LOGI("boot", "+MTREADY sent, free heap %u B",
                (unsigned)sl_memory_get_free_heap_size());
    vTaskDelete(NULL);
}

void rebuild_composition(void) {}

} // namespace

/* Runs before the kernel starts: the clocks are up (sl_clock_manager_init()
 * and the device_init steps ran earlier in sl_main_init), so the console can
 * exist before anything else logs. */
extern "C" void app_init_early(void)
{
    hearth_console_init();
    HEARTH_LOGI("boot", "Hearth on USART0 TX PA00, 115200 8N1 (sl_main)");
}

/* Runs on the sl_main start task with the kernel running. */
extern "C" void app_init(void)
{
    /* 1,280 words is 5,120 bytes, the size of the sample's own bootstrap
     * thread (kMainTaskStackSize, MatterConfig.cpp:192, where osThreadAttr_t's
     * stack_size is in BYTES and xTaskCreate's depth is in WORDS). It was
     * 1,024 words through round 1 and round 2 tasks 1 and 2, when the task's
     * deepest call was mt_at_start(); it now runs the whole of
     * hearth_matter_init(), which is the path the sample sized that thread
     * for. The task is deleted a few instructions after +MTREADY, so this is
     * not a standing cost. */
    if (xTaskCreate(hearth_boot_task, "hearth_boot", 1280, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        /* Round 1 deferral: a boot task that fails to create was silent on
         * both UARTs. Say so on the console; there is nothing else to do. */
        HEARTH_LOGE("boot", "boot task could not be created (heap %u B free)",
                    (unsigned)sl_memory_get_free_heap_size());
    }
}
