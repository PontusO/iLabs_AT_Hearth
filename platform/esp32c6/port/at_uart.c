/*
 * SPDX-FileCopyrightText: 2026 Pontus Oldberg <pontus@ilabs.se>
 * SPDX-License-Identifier: MIT
 *
 * at_uart.c - UART transport for the AT interpreter.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "at_core_config.h"
#include "hearth_port.h"

static const char *TAG = "at_uart";

static SemaphoreHandle_t s_tx_mutex;
static int s_baud = AT_UART_BAUD;
static int s_flowctrl = AT_UART_FLOWCTRL;

static uart_hw_flowcontrol_t hw_flowctrl_for(int mode)
{
    switch (mode) {
    case AT_UART_FLOWCTRL_RTS:     return UART_HW_FLOWCTRL_RTS;
    case AT_UART_FLOWCTRL_CTS:     return UART_HW_FLOWCTRL_CTS;
    case AT_UART_FLOWCTRL_CTS_RTS: return UART_HW_FLOWCTRL_CTS_RTS;
    default:                       return UART_HW_FLOWCTRL_DISABLE;
    }
}

void hearth_link_init(void)
{
#if AT_TARGET_ESP8266
    /* ESP8266_RTOS_SDK: pins are fixed by the IO mux and configured by
     * uart_param_config() itself (incl. RTS=GPIO15 / CTS=GPIO13 when
     * flow control is enabled); there is no uart_set_pin() and no
     * source_clk field. */
    uart_config_t cfg = {
        .baud_rate  = AT_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = hw_flowctrl_for(s_flowctrl),
        .rx_flow_ctrl_thresh = AT_UART_RTS_THRESH,
    };

    ESP_ERROR_CHECK(uart_driver_install(AT_UART_PORT,
                                        AT_UART_RX_BUF_SIZE,
                                        AT_UART_TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(AT_UART_PORT, &cfg));
#if AT_UART_SWAP_IO
    /* Move UART0 to GPIO15(TX)/GPIO13(RX): keeps the ROM's 74880-baud
     * boot output off the AT link. */
    ESP_ERROR_CHECK(uart_enable_swap());
#endif
#else
    const uart_config_t cfg = {
        .baud_rate  = AT_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = hw_flowctrl_for(s_flowctrl),
        .rx_flow_ctrl_thresh = AT_UART_RTS_THRESH,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(AT_UART_PORT,
                                        AT_UART_RX_BUF_SIZE,
                                        AT_UART_TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(AT_UART_PORT, &cfg));

    int rts = (AT_UART_FLOWCTRL == AT_UART_FLOWCTRL_RTS ||
               AT_UART_FLOWCTRL == AT_UART_FLOWCTRL_CTS_RTS)
                  ? AT_UART_RTS_PIN : UART_PIN_NO_CHANGE;
    int cts = (AT_UART_FLOWCTRL == AT_UART_FLOWCTRL_CTS ||
               AT_UART_FLOWCTRL == AT_UART_FLOWCTRL_CTS_RTS)
                  ? AT_UART_CTS_PIN : UART_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(uart_set_pin(AT_UART_PORT,
                                 AT_UART_TX_PIN, AT_UART_RX_PIN, rts, cts));
#endif

    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    ESP_LOGI(TAG, "AT UART%d up: %d baud, TX=%d RX=%d flowctrl=%d",
             AT_UART_PORT, AT_UART_BAUD, AT_UART_TX_PIN, AT_UART_RX_PIN,
             AT_UART_FLOWCTRL);
}

void hearth_link_write(const void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(AT_UART_PORT, data, len);
    xSemaphoreGive(s_tx_mutex);
}

void hearth_link_write_line(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return;
    }

    char *buf = malloc((size_t)need + 3);
    if (!buf) {
        ESP_LOGE(TAG, "OOM formatting line");
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    va_end(ap);
    buf[need]     = '\r';
    buf[need + 1] = '\n';

    hearth_link_write(buf, (size_t)need + 2);
    free(buf);
}

int hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    int n = uart_read_bytes(AT_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? 0 : n;
}

int hearth_link_get_baud(void)
{
    return s_baud;
}

int hearth_link_set_baud(int baud)
{
    /* Hold the TX mutex so no writer can queue more output between the
     * drain and the rate switch. */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_wait_tx_done(AT_UART_PORT, pdMS_TO_TICKS(1000));

    esp_err_t err = uart_set_baudrate(AT_UART_PORT, (uint32_t)baud);
    if (err == ESP_OK) {
        s_baud = baud;
    }
    xSemaphoreGive(s_tx_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "baud change to %d failed: %s", baud, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "AT UART baud now %d", baud);
    return 0;
}

int hearth_link_get_flowctrl(void)
{
    return s_flowctrl;
}

int hearth_link_set_flowctrl(int mode)
{
    if (mode < AT_UART_FLOWCTRL_NONE || mode > AT_UART_FLOWCTRL_CTS_RTS) {
        return -1;
    }

    /* Hold the TX mutex and drain first so the "OK" already queued at the
     * old state leaves before flow control engages (enabling CTS would
     * otherwise gate this device's transmitter on the host's readiness). */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_wait_tx_done(AT_UART_PORT, pdMS_TO_TICKS(1000));

    esp_err_t err = ESP_OK;

#if !AT_TARGET_ESP8266
    /* Route the RTS/CTS GPIOs for the requested mode (or leave them alone
     * when the corresponding signal is unused). TX/RX stay put. On C3/C6
     * all four pins go through the GPIO matrix, so this works even when
     * flow control was disabled at build time. */
    int rts = (mode == AT_UART_FLOWCTRL_RTS || mode == AT_UART_FLOWCTRL_CTS_RTS)
                  ? AT_UART_RTS_PIN : UART_PIN_NO_CHANGE;
    int cts = (mode == AT_UART_FLOWCTRL_CTS || mode == AT_UART_FLOWCTRL_CTS_RTS)
                  ? AT_UART_CTS_PIN : UART_PIN_NO_CHANGE;
    err = uart_set_pin(AT_UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       rts, cts);
#endif

    if (err == ESP_OK) {
        err = uart_set_hw_flow_ctrl(AT_UART_PORT, hw_flowctrl_for(mode),
                                    AT_UART_RTS_THRESH);
    }
    if (err == ESP_OK) {
        s_flowctrl = mode;
    }
    xSemaphoreGive(s_tx_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flow control change to %d failed: %s",
                 mode, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "AT UART flow control now %d", mode);
    return 0;
}
