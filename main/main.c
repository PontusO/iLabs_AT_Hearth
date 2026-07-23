/*
 * main.c - iLabs AT Matter interpreter entry point (skeleton, Phase B1).
 *
 * Proves the shared at_core engine is reusable in a second image: bring up
 * the AT UART, register the Matter command table, start the parser. No
 * radio and no Matter stack yet - those arrive in Phase B2/B3 through the
 * same link_mgr the ESP-NOW firmware uses.
 */

#include "esp_log.h"

#include "mt_at_config.h"
#include "at_uart.h"
#include "at_parser.h"
#include "mt_at.h"

void app_main(void)
{
    at_uart_init();
    mt_at_register();
    at_parser_start(&mt_at_engine_cfg);

    /* Boot marker so the host can synchronize after a slave reset,
     * mirroring the ESP-NOW firmware's +ENREADY. */
    at_uart_write_line("+MTREADY");

    ESP_LOGI("main", "iLabs AT Matter %s ready", MT_FW_VERSION);
}
