/*
 * hearth_port_esp.c - hearth_port.h and hearth_log.h on ESP-IDF.
 * The link half lives in at_uart.c (same component); this file wraps
 * OS, KV and log.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#include "hearth_log.h"
#include "hearth_port.h"

void hearth_os_sleep_ms(uint32_t ms)  { vTaskDelay(pdMS_TO_TICKS(ms)); }
void hearth_os_restart(void)          { esp_restart(); }

/* AT+CGMM / AT+GMM model string (hearth_port.h). This is the value the
 * shared core used to hardcode for every build; kept identical here so the
 * C6's wire answer does not move. */
const char *hearth_port_model(void)   { return "ESP32-C6 Hearth"; }

/* ---- bulk working memory (ruling DE419) --------------------------------
 *
 * The ordinary allocator, which on ESP-IDF is the general internal heap and
 * is the same one esp_matter and CHIP already use. That is the right answer
 * HERE and the wrong one on the nRF54L15, where the application is linked
 * with --wrap=malloc and plain malloc() resolves to a private 10,240-byte
 * Matter working heap; see hearth_port.h for what an implementation has to
 * satisfy and hearth_port_zephyr.c for what that arm had to do instead.
 * This image has around 106 KB free at a one-endpoint composition and about
 * 47 KB at twenty-eight, so a 5,608-byte staging session is unremarkable
 * and shares a heap that is sized for sharing.
 *
 * FreeRTOS-IDF's malloc is task-safe, which is what the header requires:
 * the host stage is allocated on the AT parser task and the inbound stage
 * is committed on whichever task runs the composition rebuild.
 */
void *hearth_stage_alloc(size_t bytes)
{
    return malloc(bytes);
}

void hearth_stage_free(void *block)
{
    free(block);
}

int hearth_os_task_spawn(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, unsigned prio)
{
    return xTaskCreate(fn, name, stack_bytes, arg, prio, NULL) == pdPASS ? 0 : -1;
}

hearth_sem_t hearth_sem_create_binary(void)
{
    return (hearth_sem_t)xSemaphoreCreateBinary();
}

bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)sem,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void hearth_sem_give(hearth_sem_t sem)
{
    xSemaphoreGive((SemaphoreHandle_t)sem);
}

static portMUX_TYPE s_crit[HEARTH_CRIT_COUNT] = {
    portMUX_INITIALIZER_UNLOCKED,
    portMUX_INITIALIZER_UNLOCKED,
};

void hearth_crit_enter(int id) { taskENTER_CRITICAL(&s_crit[id]); }
void hearth_crit_exit(int id)  { taskEXIT_CRITICAL(&s_crit[id]); }

int hearth_kv_get_blob(const char *ns, const char *key,
                       void *buf, size_t *inout_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (err != ESP_OK) return -1;
    err = nvs_get_blob(h, key, buf, inout_len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    return err == ESP_OK ? 0 : -1;
}

int hearth_kv_set_blob(const char *ns, const char *key,
                       const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int hearth_kv_get_u8(const char *ns, const char *key, uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (err != ESP_OK) return -1;
    err = nvs_get_u8(h, key, out);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    return err == ESP_OK ? 0 : -1;
}

int hearth_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int hearth_kv_delete(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (err != ESP_OK) return -1;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    return err == ESP_OK ? 0 : -1;
}

void hearth_log_write(hearth_log_level_t level, const char *tag,
                      const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    switch (level) {
    case HEARTH_LOG_ERROR: ESP_LOGE(tag, "%s", line); break;
    case HEARTH_LOG_WARN:  ESP_LOGW(tag, "%s", line); break;
    default:               ESP_LOGI(tag, "%s", line); break;
    }
}
