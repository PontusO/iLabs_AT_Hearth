/*
 * main.c - Silabs boot path, skeleton round. Boot contract per the layout
 * spec section 5 and the MG24 port spec section 6: platform up, then
 * mt_at_start(), which emits +MTREADY. No URC precedes the boot marker.
 * The Matter stack and the composition rebuild arrive with the upward
 * port round; this skeleton answers the AT surface with the stubs.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "sl_system_init.h"
#include "sl_system_kernel.h"

#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_at.h"

/* port/hearth_port_sl.c. Declared at its one call site rather than in a
 * header of its own: the console has exactly one caller, main(). */
void hearth_console_init(void);

/* mt_at_start() spawns the parser task through hearth_os_task_spawn() and
 * returns; that spawn needs the scheduler, so the boot marker is emitted
 * from a task rather than from main().
 *
 * mt_at_start() calls hearth_link_init() itself (core/mt/mt_at.c), so this
 * does not, the way platform/nrf54l15/src/main.cpp does not: a second call
 * would create a second RX semaphore and a second TX mutex and leak the
 * first pair, and would reconfigure EUSART0 underneath a link that is
 * already up. */
static void hearth_boot_task(void *arg)
{
    (void)arg;
    HEARTH_LOGI("boot", "boot task up, model %s", hearth_port_model());
    mt_at_start();          /* emits +MTREADY once the parser task is up */
    HEARTH_LOGI("boot", "+MTREADY sent, free heap %u B",
                (unsigned)xPortGetFreeHeapSize());
    vTaskDelete(NULL);
}

int main(void)
{
    sl_system_init();
    hearth_console_init();  /* after sl_system_init(): it brings the clocks up */
    HEARTH_LOGI("boot", "Hearth skeleton on USART0 TX PA00, 115200 8N1");
    xTaskCreate(hearth_boot_task, "hearth_boot", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
    /* sl_system_kernel_start(), not vTaskStartScheduler() directly. It
     * fires the generated kernel_start event, and on this project that
     * handler is osKernelStart() (autogen/sl_event_handler.c): the
     * CMSIS-RTOS2 entry that pairs with the osKernelInitialize()
     * sl_platform_init() already ran. Calling the FreeRTOS scheduler
     * straight would step around both, and would silently drop any
     * kernel_start handler a later component registers. It does not
     * return. */
    sl_system_kernel_start();
    for (;;) {}             /* the scheduler never returns */
}
