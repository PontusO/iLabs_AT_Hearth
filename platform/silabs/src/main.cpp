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
 *                     sl_driver_init, sl_service_init, sl_stack_init).
 *
 * Nothing here starts the scheduler.
 *
 * Boot contract (layout spec section 5, MG24 spec section 6): platform up,
 * console, then mt_at_start() on a task, which emits +MTREADY. No URC
 * precedes the marker. The Matter stack init lands in hearth_boot_task()
 * in Task 3 of the matter-core plan, ahead of mt_at_start().
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

#include "hearth_console.h"
#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_at.h"

extern "C" void app_init_early(void);
extern "C" void app_init(void);

namespace {

/* mt_at_start() calls hearth_link_init() itself (core/mt/mt_at.c); a second
 * call here would leak the RX semaphore and TX mutex and reconfigure EUSART0
 * under a live link. */
void hearth_boot_task(void *arg)
{
    (void)arg;
    HEARTH_LOGI("boot", "boot task up, model %s", hearth_port_model());
    mt_at_start();          /* emits +MTREADY once the parser task is up */
    HEARTH_LOGI("boot", "+MTREADY sent, free heap %u B",
                (unsigned)sl_memory_get_free_heap_size());
    vTaskDelete(NULL);
}

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
    if (xTaskCreate(hearth_boot_task, "hearth_boot", 1024, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        /* Round 1 deferral: a boot task that fails to create was silent on
         * both UARTs. Say so on the console; there is nothing else to do. */
        HEARTH_LOGE("boot", "boot task could not be created (heap %u B free)",
                    (unsigned)sl_memory_get_free_heap_size());
    }
}
