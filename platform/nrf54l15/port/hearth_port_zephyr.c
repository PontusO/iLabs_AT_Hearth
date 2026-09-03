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
#include <zephyr/sys/ring_buffer.h>

#include "hearth_log.h"
#include "hearth_port.h"

LOG_MODULE_REGISTER(hearth, LOG_LEVEL_INF);

/* The AT link is a per-board devicetree fact and there is deliberately no
 * default: uart20 is the AT link on ophelia_cpico and the CONSOLE on both
 * Nordic DKs, so a guess would answer AT on the wrong pins without failing.
 * platform/nrf54l15/CMakeLists.txt says the same thing earlier and at more
 * length; this is the backstop for a build that reaches the compiler
 * anyway. Without it, an unresolved DT_CHOSEN() pastes its own macro name
 * into the device symbol below and the diagnostic is an undeclared
 * "__device_dts_ord_DT_CHOSEN_..._ORD" with six expansion notes in headers
 * nobody wrote, naming neither a board nor an overlay. */
#if !DT_HAS_CHOSEN(hearth_at_uart)
#error "This board declares no hearth,at-uart chosen node, so nothing says which UART carries the AT link. Add `/ { chosen { hearth,at-uart = &uartNN; }; };` and that UART's status to the board devicetree, and name the same node as zephyr,uart-mcumgr in platform/nrf54l15/sysbuild/mcuboot/boards/<board>.overlay. See platform/nrf54l15/README.md, \"Adding a board\"."
#endif

static const struct device *s_uart =
    DEVICE_DT_GET(DT_CHOSEN(hearth_at_uart));
static struct k_mutex s_tx_lock;

void hearth_os_sleep_ms(uint32_t ms) { k_msleep(ms); }
void hearth_os_restart(void)         { sys_reboot(SYS_REBOOT_WARM); }

/* AT+CGMM / AT+GMM model string (hearth_port.h). Selected from the SoC the
 * image is built for, so a board sitting on either supported part answers
 * with its own chip rather than the shared core's old "ESP32-C6" constant.
 * The choice is on the SoC, not the board (CONFIG_BOARD): the three boards
 * this port serves are two SoCs (ophelia_cpico and nrf54l15dk are
 * nRF54L15; nrf54lm20dk is nRF54LM20A), and the model names the
 * co-processor. A future SoC in this port MUST add its arm here; the
 * #error makes that a build failure rather than a wrong-but-plausible
 * answer, the failure mode the board contract exists to avoid. */
const char *hearth_port_model(void)
{
#if defined(CONFIG_SOC_NRF54L15)
	return "nRF54L15 Hearth";
#elif defined(CONFIG_SOC_NRF54LM20A)
	return "nRF54LM20A Hearth";
#else
#error "hearth_port_model(): unknown nRF SoC; add its model string arm"
#endif
}

/* ---- bulk working memory (ruling DE419) --------------------------------
 *
 * THE ONE THING TO KNOW BEFORE EDITING THIS: on this platform plain
 * malloc() IS NOT THE LIBC HEAP. CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE=y
 * links the application with --wrap=malloc/free/calloc/realloc, and
 * connectedhomeip's src/platform/Zephyr/SysHeapMalloc.cpp aliases
 * __wrap_malloc onto its own sys_heap over a static
 * uint8_t sHeapMemory[CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE], 10,240 bytes here,
 * which is where the ENTIRE Matter stack allocates from: sessions,
 * exchanges, attribute paths, TLV, mDNS. A 5,608-byte row staging session
 * taken through malloc() would hold 55% of it for as long as a host leaves
 * a set staged, and exhaustion there is a commissioning failure. Staging
 * must never be able to starve the stack, and the stack must never be able
 * to fail a stage.
 *
 * So this takes the libc arena instead, by name. __real_malloc is the
 * unwrapped common-libc malloc that --wrap leaves reachable, and with
 * CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1 its arena is every byte of SRAM
 * the image does not statically use (malloc.c: HEAP_BASE is _end rounded
 * up, running to the end of SRAM). It GROWS by exactly what moving the
 * staging buffers out of .bss freed, so the round's reclaim and the round's
 * spend are the same memory. Nothing else in the image draws on it,
 * precisely because every other malloc in the link is wrapped away to
 * CHIP's heap.
 *
 * Two consequences worth stating rather than rediscovering:
 *
 *   The lock is not CHIP's. Zephyr's common-libc malloc takes its own
 *   z_malloc_heap_mutex (lib/libc/common/source/stdlib/malloc.c:140),
 *   which the Matter stack never takes because its allocations are wrapped
 *   elsewhere. The AT parser task therefore does not acquire a CHIP-owned
 *   mutex on the AT+MTROW path, which going through malloc() would have
 *   made it do.
 *
 *   The dependency fails loudly if it ever stops holding. Without --wrap,
 *   __real_malloc is an undefined symbol and the link fails; the #error
 *   below turns that into a readable message at compile time instead.
 *
 * ---- A STANDING CONDITION, TRUE TODAY AND ENFORCED BY NOTHING ----
 *
 * "Nothing else in the image draws on this arena" is a fact about today's
 * link, not a property the build maintains. It was checked at the
 * disassembly level for round B's fix review: the only callers of the
 * unwrapped libc malloc and free anywhere in the image are the two
 * functions below. Nothing keeps that true.
 *
 * The wrap set is exactly malloc, calloc, realloc and free, plus their
 * _malloc_r / _calloc_r / _realloc_r / _free_r reentrant forms
 * (SysHeapMalloc.cpp's WRAP block, and the --wrap flags on the app link).
 * Everything ELSE the common-libc allocator exports reaches THIS arena
 * without passing through CHIP at all: aligned_alloc, memalign,
 * posix_memalign, reallocarray, strdup and strndup. A future caller of any
 * of them, in this application or in a Zephyr subsystem linked into it,
 * silently becomes a second tenant of the staging arena, and the arithmetic
 * in ../README.md's round B section quietly stops describing the memory it
 * claims to describe.
 *
 * Nothing in the compiler, the linker or the build gates catches that. The
 * call succeeds, the firmware works, and the only symptom is a staging
 * allocation that fails on a loaded device one day. So it is written down
 * where a person changing this file will read it: if the image ever needs
 * one of those functions, either wrap it too, or size and measure this
 * arena as a shared resource rather than a dedicated one. Checking means
 * reading the map or the disassembly, not grepping the sources, because the
 * names can arrive through a library.
 */
#ifndef CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE
#error "hearth_stage_alloc() takes the libc arena through __real_malloc, which exists \
only because CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE links this application with \
--wrap=malloc. Without the wrap, plain malloc() IS the libc arena, so the fix is to drop \
the __real_ prefix. Do not delete this guard without establishing which heap malloc() \
now resolves to: getting that question wrong is what this whole comment is about."
#endif

extern void *__real_malloc(size_t bytes);
extern void  __real_free(void *block);

void *hearth_stage_alloc(size_t bytes)
{
    return __real_malloc(bytes);
}

void hearth_stage_free(void *block)
{
    __real_free(block);
}

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

/* ---- link RX (interrupt-driven) -----------------------------------
 * TX stays uart_poll_out: the UARTE TX FIFO is drained by hearth code
 * one byte at a time and nothing is lost sitting in it. RX is the other
 * direction: this driver (UART_NRFX_UARTE_LEGACY_SHIM) does not FIFO
 * more than a single staging byte at 115200 (poll_in_byte, ENDRX taken
 * per byte, STARTRX re-armed inside uart_fifo_read), and the old
 * uart_poll_in + k_msleep(1) loop dropped bytes that arrived during the
 * sleep window (a bench-proven defect: "AT+MTEP=256" lost its '=' and
 * was answered as an unknown command). The ISR below drains that single
 * byte into a ring buffer as fast as it lands; hearth_link_read only
 * ever touches the ring, never the FIFO directly. Per-byte re-arm has
 * no headroom above 115200 without moving to the async API; tracked as
 * a graph node, not addressed here. */

static uint8_t s_rx_buf[1024]; /* MT_AT_LINE_MAX is 512; headroom is cheap */
static struct ring_buf s_rx_ring;
static struct k_sem s_rx_sem;
/* ISR-increment-only; hearth_link_read (thread context) compares this
 * against s_rx_overflow_seen and logs the delta. No logging from ISR
 * context. */
static uint32_t s_rx_overflow_count;
static uint32_t s_rx_overflow_seen;

static void hearth_link_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        while (uart_fifo_read(dev, &byte, 1) == 1) {
            if (ring_buf_put(&s_rx_ring, &byte, 1) == 0) {
                /* Ring full: drop the incoming byte, count it. The
                 * count is drained and logged from thread context. */
                s_rx_overflow_count++;
            }
        }
    }
    k_sem_give(&s_rx_sem);
}

void hearth_link_init(void)
{
    k_mutex_init(&s_tx_lock);
    ring_buf_init(&s_rx_ring, sizeof(s_rx_buf), s_rx_buf);
    k_sem_init(&s_rx_sem, 0, 1);
    uart_irq_callback_user_data_set(s_uart, hearth_link_isr, NULL);
    uart_irq_rx_enable(s_uart);
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
    /*
     * A deadline loop, not a single sem_take: the ISR gives the
     * semaphore once per interrupt entry, including entries where the
     * drain leaves the ring empty (already-consumed data, a TX-side
     * interrupt on the same callback), so one successful take does not
     * guarantee bytes. Loop until len bytes are accumulated or the
     * deadline passes, matching the ESP port (uart_read_bytes) and the
     * parser's contract (at_parser.c parser_task: "the link read waits
     * for the FULL length"). timeout_ms 0 keeps poll semantics: a
     * single non-blocking attempt, no deadline loop.
     *
     * K_MSEC(uint32_t) is a real blocking wait, not a poll: k_sem_take
     * parks this thread and the kernel wakes it on either a give or the
     * timeout, no busy-waiting. The remaining-time computation below
     * stays within uint32_t range (clamped), so K_MSEC never overflows,
     * even for the header's 3,600,000 ms (one hour) floor.
     */
    int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
    size_t got = 0;

    while (got < len) {
        k_timeout_t wait;

        if (timeout_ms == 0) {
            wait = K_NO_WAIT;
        } else {
            int64_t left = deadline - k_uptime_get();
            if (left <= 0) break;
            wait = K_MSEC((uint32_t)(left > UINT32_MAX ? UINT32_MAX : left));
        }

        if (k_sem_take(&s_rx_sem, wait) != 0) {
            if (timeout_ms == 0) break;
            continue; /* deadline re-checked at the top of the loop */
        }

        got += ring_buf_get(&s_rx_ring, buf + got, len - got);

        /* More queued than we took (len was smaller); re-give so the
         * next lap, or the next call, does not block spuriously. */
        if (!ring_buf_is_empty(&s_rx_ring)) k_sem_give(&s_rx_sem);

        uint32_t overflow = s_rx_overflow_count;
        if (overflow != s_rx_overflow_seen) {
            HEARTH_LOGW("link", "rx ring overflow, dropped %u byte(s)",
                       (unsigned)(overflow - s_rx_overflow_seen));
            s_rx_overflow_seen = overflow;
        }
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
    uint32_t old_baud = cfg.baudrate;
    cfg.baudrate = (uint32_t)baud;

    /*
     * cmd_mtbaud (mt_at.c) documents the contract this must honor: the
     * AT+MTBAUD OK is written at the OLD rate and hearth_link_set_baud()
     * drains TX before switching, so the host (still listening at the
     * old rate) sees a clean OK, not tail bytes at the new rate. Hold
     * the tx lock across drain-and-switch so no later write queues into
     * the gap, and busy-wait two character times (10 bits/char: start +
     * 8 data + stop) at the OLD baud so the OK is actually off the wire,
     * not just handed to the UARTE, before the rate changes under it.
     */
    k_mutex_lock(&s_tx_lock, K_FOREVER);

    uint32_t drain_us = (old_baud != 0)
        ? (uint32_t)(2ULL * 10 * 1000000 / old_baud) + 100
        : 100;
    k_busy_wait(drain_us);

    uart_irq_rx_disable(s_uart);
    if (uart_configure(s_uart, &cfg) != 0) {
        uart_irq_rx_enable(s_uart);
        k_mutex_unlock(&s_tx_lock);
        return -1;
    }

    /* A byte that arrived mid-reconfigure was sampled at the wrong rate
     * and is garbage; the sem count carried over from before the switch
     * is stale for the same reason. Flush both while the rx irq is
     * still off, so this cannot race the ISR. */
    ring_buf_reset(&s_rx_ring);
    k_sem_reset(&s_rx_sem);

    uart_irq_rx_enable(s_uart);
    k_mutex_unlock(&s_tx_lock);
    return 0;
}

int hearth_link_get_flowctrl(void) { return 0; }
int hearth_link_set_flowctrl(int mode) { return mode == 0 ? 0 : -1; }

/* ---- persistent KV (Zephyr settings over ZMS) --------------------- *
 * The settings_storage partition is mapped by pm_static.yml. This build
 * has CONFIG_PARTITION_MANAGER_ENABLED=y, so zephyr/include/zephyr/
 * storage/flash_map.h takes its USE_PARTITION_MANAGER branch and pulls
 * in nrf/include/flash_map_pm.h instead of the plain devicetree-based
 * FIXED_PARTITION_ID. Because CONFIG_SETTINGS_ZMS is set, flash_map_pm.h
 * preprocessor-aliases the bare token storage_partition to
 * settings_storage (`#define storage_partition settings_storage`), and
 * redefines FIXED_PARTITION_ID(label) as PM_ID(label), i.e.
 * PM_##label##_ID. So subsys/settings/src/settings_zms.c's fallback
 * FIXED_PARTITION_ID(storage_partition) expands, via that token alias,
 * to PM_settings_storage_ID, i.e. PM_SETTINGS_STORAGE_ID straight out of
 * the generated pm_config.h (0x15C000, size 0x8000). Devicetree is never
 * consulted on this path; the "storage_partition" nodelabel that also
 * happens to live in boards/ilabs/ophelia_cpico/
 * ophelia_cpico_nrf54l15_cpuapp.dts (address 0x15c000, but a stale,
 * PM-ignored 0x9000 size) is not what resolves this and its own
 * DT-derived partition ID matching PM's is coincidence, not the
 * mechanism. Do NOT "fix" this by adding a zephyr,settings-partition
 * chosen node: that only flips settings_zms.c onto its other branch
 * (DT_FIXED_PARTITION_ID via DT_CHOSEN, the raw devicetree-ordinal path,
 * bypassing flash_map_pm.h's alias entirely), which is unnecessary
 * because the PM alias above already binds correctly, and swaps a
 * working, PM-address-accurate path for one keyed off devicetree
 * ordinals that are not guaranteed to track pm_static.yml.
 */

#define KV_PATH_MAX 48

/* settings_subsys_init() is called once, lazily, on the first hearth_kv_*
 * call (no dedicated boot hook). A failed init does not latch, so a later
 * call retries instead of being wedged for the life of the process.
 * hearth_kv_* is only ever called from one thread today; if that ever
 * changes, this still holds, because settings_subsys_init() takes the
 * settings subsystem's own internal mutex and is idempotent to call more
 * than once (subsys/settings/src/settings_init.c). */
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
