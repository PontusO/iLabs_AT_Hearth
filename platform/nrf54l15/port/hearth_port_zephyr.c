/*
 * hearth_port_zephyr.c - hearth_port.h and hearth_log.h on Zephyr.
 * Skeleton quality: the KV store is RAM-backed (the real port moves to
 * ZMS/settings in the Nordic round), flow control reports none.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "hearth_log.h"
#include "hearth_port.h"

LOG_MODULE_REGISTER(hearth, LOG_LEVEL_INF);

static const struct device *s_uart =
    DEVICE_DT_GET(DT_CHOSEN(hearth_at_uart));
static struct k_mutex s_tx_lock;

void hearth_os_sleep_ms(uint32_t ms) { k_msleep(ms); }
void hearth_os_restart(void)         { sys_reboot(SYS_REBOOT_WARM); }

int hearth_os_task_spawn(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, unsigned prio)
{
    k_thread_stack_t *stack = k_thread_stack_alloc(stack_bytes, 0);
    if (stack == NULL) return -1;
    /* One TCB per call, not a shared static: a second spawn while an
     * earlier thread is still live must not reinitialize its TCB out
     * from under it. */
    struct k_thread *thread = k_malloc(sizeof(*thread));
    if (thread == NULL) {
        k_thread_stack_free(stack);
        return -1;
    }
    k_tid_t tid = k_thread_create(thread, stack, stack_bytes,
                                  (k_thread_entry_t)fn, arg, NULL, NULL,
                                  K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    if (tid == NULL) {
        k_free(thread);
        k_thread_stack_free(stack);
        return -1;
    }
    k_thread_name_set(tid, name);
    return 0;
}

hearth_sem_t hearth_sem_create_binary(void)
{
    static struct k_sem sems[4];
    static int next;
    if (next >= 4) return NULL;
    k_sem_init(&sems[next], 0, 1);
    return &sems[next++];
}

bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms)
{
    return k_sem_take((struct k_sem *)sem, K_MSEC(timeout_ms)) == 0;
}

void hearth_sem_give(hearth_sem_t sem)
{
    k_sem_give((struct k_sem *)sem);
}

/* Skeleton critical sections: spinlocks, keys stored per section (the
 * core never nests a section with itself). */
static struct k_spinlock s_crit[HEARTH_CRIT_COUNT];
static k_spinlock_key_t s_crit_key[HEARTH_CRIT_COUNT];

void hearth_crit_enter(int id) { s_crit_key[id] = k_spin_lock(&s_crit[id]); }
void hearth_crit_exit(int id)  { k_spin_unlock(&s_crit[id], s_crit_key[id]); }

void hearth_link_init(void)
{
    k_mutex_init(&s_tx_lock);
}

void hearth_link_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    k_mutex_lock(&s_tx_lock, K_FOREVER);
    for (size_t i = 0; i < len; i++)
        uart_poll_out(s_uart, p[i]);
    k_mutex_unlock(&s_tx_lock);
}

void hearth_link_write_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = sizeof(line) - 2;
    line[n] = '\r'; line[n + 1] = '\n';
    hearth_link_write(line, n + 2);
}

int hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    int64_t end = k_uptime_get() + timeout_ms;
    while (got < len) {
        unsigned char c;
        if (uart_poll_in(s_uart, &c) == 0) {
            buf[got++] = c;
            continue;
        }
        if (k_uptime_get() >= end) break;
        k_msleep(1);
    }
    return (int)got;
}

int hearth_link_get_baud(void)
{
    struct uart_config cfg;
    if (uart_config_get(s_uart, &cfg) != 0) return 115200;
    return (int)cfg.baudrate;
}

int hearth_link_set_baud(int baud)
{
    struct uart_config cfg;
    if (uart_config_get(s_uart, &cfg) != 0) return -1;
    cfg.baudrate = (uint32_t)baud;
    return uart_configure(s_uart, &cfg) == 0 ? 0 : -1;
}

int hearth_link_get_flowctrl(void) { return 0; }
int hearth_link_set_flowctrl(int mode) { return mode == 0 ? 0 : -1; }

/* ---- RAM KV (skeleton; ZMS in the Nordic round) ------------------ */

#define KV_MAX 8

static struct {
    char    ns[16], key[16];
    uint8_t blob[512];
    size_t  len;
    int     used;
} s_kv[KV_MAX];

static int kv_find(const char *ns, const char *key)
{
    for (int i = 0; i < KV_MAX; i++)
        if (s_kv[i].used && !strcmp(s_kv[i].ns, ns) && !strcmp(s_kv[i].key, key))
            return i;
    return -1;
}

int hearth_kv_get_blob(const char *ns, const char *key,
                       void *buf, size_t *inout_len)
{
    int i = kv_find(ns, key);
    if (i < 0) return 1;
    if (*inout_len < s_kv[i].len) return -1;
    memcpy(buf, s_kv[i].blob, s_kv[i].len);
    *inout_len = s_kv[i].len;
    return 0;
}

int hearth_kv_set_blob(const char *ns, const char *key,
                       const void *buf, size_t len)
{
    if (len > sizeof(s_kv[0].blob)) return -1;
    int i = kv_find(ns, key);
    if (i < 0)
        for (int j = 0; j < KV_MAX; j++)
            if (!s_kv[j].used) { i = j; break; }
    if (i < 0) return -1;
    snprintf(s_kv[i].ns, sizeof(s_kv[i].ns), "%s", ns);
    snprintf(s_kv[i].key, sizeof(s_kv[i].key), "%s", key);
    memcpy(s_kv[i].blob, buf, len);
    s_kv[i].len = len;
    s_kv[i].used = 1;
    return 0;
}

int hearth_kv_get_u8(const char *ns, const char *key, uint8_t *out)
{
    size_t len = 1;
    return hearth_kv_get_blob(ns, key, out, &len);
}

int hearth_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    return hearth_kv_set_blob(ns, key, &val, 1);
}

int hearth_kv_delete(const char *ns, const char *key)
{
    int i = kv_find(ns, key);
    if (i < 0) return 1;
    s_kv[i].used = 0;
    return 0;
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
    case HEARTH_LOG_ERROR: LOG_ERR("%s: %s", tag, line); break;
    case HEARTH_LOG_WARN:  LOG_WRN("%s: %s", tag, line); break;
    default:               LOG_INF("%s: %s", tag, line); break;
    }
}
