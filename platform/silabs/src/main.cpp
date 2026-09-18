/*
 * main.cpp - the sl_main hooks and Hearth's bootstrap task.
 *
 * Under sl_main with a kernel the SDK owns main(): main_retarget.c wraps
 * it, runs sl_main_init(), calls app_init_early() and app_init() from the
 * start task, and starts the scheduler. Nothing here starts the scheduler
 * and nothing here is called before the kernel exists.
 *
 * Boot contract (layout spec section 5, MG24 spec section 6): platform up,
 * console, then mt_at_start() on a task, which emits +MTREADY. No URC
 * precedes the marker. The Matter stack init lands in hearth_boot_task()
 * in Task 3 of the matter-core plan, ahead of mt_at_start().
 */

#include "FreeRTOS.h"
#include "task.h"

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
    HEARTH_LOGI("boot", "+MTREADY sent, free FreeRTOS heap %u B",
                (unsigned)xPortGetFreeHeapSize());
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
        HEARTH_LOGE("boot", "boot task could not be created (FreeRTOS heap %u B free)",
                    (unsigned)xPortGetFreeHeapSize());
    }
}
