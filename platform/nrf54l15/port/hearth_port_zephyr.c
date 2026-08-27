/*
 * hearth_port_zephyr.c - hearth_port.h and hearth_log.h on Zephyr.
 * The KV store persists through the settings subsystem on the ZMS
 * backend (settings_storage partition, pm_static.yml); flow control
 * still reports none.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
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

/* ---- persistent KV (Zephyr settings over ZMS) --------------------- *
 * The settings_storage partition is mapped by pm_static.yml; the ZMS
 * backend binds to it via the "storage_partition" devicetree nodelabel
 * (boards/ilabs/ophelia_cpico/ophelia_cpico_nrf54l15_cpuapp.dts),
 * matched by subsys/settings/src/settings_zms.c's
 * FIXED_PARTITION_ID(storage_partition) fallback when no
 * "zephyr,settings-partition" chosen node is present.
 */

#define KV_PATH_MAX 48

/* settings_subsys_init() is called once, lazily, on the first hearth_kv_*
 * call (no dedicated boot hook). A failed init does not latch, so a later
 * call retries instead of being wedged for the life of the process. */
static bool s_kv_ready;

static int kv_ensure_init(void)
{
    if (s_kv_ready) return 0;
    if (settings_subsys_init() != 0) return -1;
    s_kv_ready = true;
    return 0;
}

/* ns and key are short, well-known strings ("mt_ep"/"comp",
 * "mt_cfg"/"transport"); refuse rather than silently truncate. */
static int kv_path(char *out, size_t out_len, const char *ns, const char *key)
{
    int n = snprintf(out, out_len, "hearth/%s/%s", ns, key);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

struct kv_read_ctx {
    void   *buf;     /* NULL for an existence-only probe (hearth_kv_delete) */
    size_t  cap;
    size_t  len;
    bool    found;
    bool    too_big;
};

/*
 * settings_load_subtree_direct() invokes this with the matched key
 * RELATIVE to the subtree argument we pass (the full "hearth/ns/key"
 * path). Verified against ~/ncs/v3.0.2/zephyr/subsys/settings/src/
 * settings.c: settings_call_set_handler() computes the relative key with
 * settings_name_steq(name, subtree, &name_key); that function only
 * assigns *next (our relative key) when a '/' separator follows the
 * matched prefix, and leaves it at its initialized NULL when name equals
 * subtree exactly (subsys/settings/src/settings.c:74-110). So for our
 * exact-path subtree the one leaf we asked for calls back with key ==
 * NULL; any deeper node (there are none here, but the check is cheap and
 * correct if the tree ever grows) calls back with a non-empty key. The
 * same NULL-or-empty check is how NCS's own settings-backed persistent
 * storage does it (nrf/samples/matter/common/src/persistent_storage/
 * backends/persistent_storage_settings.cpp, LoadEntryCallback).
 */
static int kv_read_cb(const char *key, size_t len, settings_read_cb read_cb,
                      void *cb_arg, void *param)
{
    struct kv_read_ctx *ctx = param;

    if (key != NULL && key[0] != '\0') return 0;

    ctx->found = true;
    ctx->len = len;
    if (ctx->buf == NULL) return 1; /* existence probe, no read needed */
    if (len > ctx->cap) { ctx->too_big = true; return 1; }
    ssize_t n = read_cb(cb_arg, ctx->buf, len);
    if (n < 0) { ctx->found = false; return 1; }
    ctx->len = (size_t)n;
    return 1; /* exact-path subtree: one leaf at most, stop here */
}

int hearth_kv_get_blob(const char *ns, const char *key,
                       void *buf, size_t *inout_len)
{
    char path[KV_PATH_MAX];
    if (kv_ensure_init() != 0) return -1;
    if (kv_path(path, sizeof(path), ns, key) != 0) return -1;

    struct kv_read_ctx ctx = { .buf = buf, .cap = *inout_len };
    settings_load_subtree_direct(path, kv_read_cb, &ctx);
    if (!ctx.found) return 1;
    if (ctx.too_big) return -1;
    *inout_len = ctx.len;
    return 0;
}

int hearth_kv_set_blob(const char *ns, const char *key,
                       const void *buf, size_t len)
{
    char path[KV_PATH_MAX];
    if (kv_ensure_init() != 0) return -1;
    if (kv_path(path, sizeof(path), ns, key) != 0) return -1;
    return settings_save_one(path, buf, len) == 0 ? 0 : -1;
}

int hearth_kv_get_u8(const char *ns, const char *key, uint8_t *out)
{
    size_t len = sizeof(*out);
    return hearth_kv_get_blob(ns, key, out, &len);
}

int hearth_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    return hearth_kv_set_blob(ns, key, &val, sizeof(val));
}

int hearth_kv_delete(const char *ns, const char *key)
{
    char path[KV_PATH_MAX];
    if (kv_ensure_init() != 0) return -1;
    if (kv_path(path, sizeof(path), ns, key) != 0) return -1;

    struct kv_read_ctx ctx = { .buf = NULL };
    settings_load_subtree_direct(path, kv_read_cb, &ctx);
    if (!ctx.found) return 1;
    return settings_delete(path) == 0 ? 0 : -1;
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
