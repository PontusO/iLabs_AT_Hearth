/*
 * hearth_port.h - the downward port: everything the portable core needs
 * from an OS/SDK, implemented once per platform (spec section 4.1).
 * Return convention: 0 ok, -1 error; hearth_kv_get_* return 1 when the
 * key does not exist (absent is not an error, mt_comp_store depends on
 * the distinction).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- identity ---------------------------------------------------- *
 *
 * The co-processor model string, answered verbatim by AT+CGMM and AT+GMM
 * (AT_MT_SPEC.md section 3.1). It names the co-processor, so it is a
 * PLATFORM fact, not a portable one: an ESP32-C6 build answers
 * "ESP32-C6 Hearth" and an nRF build answers its own SoC, and a shared
 * core constant would make one of them lie. The manufacturer string is
 * portable (iLabs on every build) and stays a core constant
 * (MT_MANUFACTURER, mt_at_config.h); only the model crosses this
 * boundary. Returns a stable, NUL-terminated string with static storage
 * duration; the core does not free it and may hold the pointer.
 */
const char *hearth_port_model(void);

/* ---- OS ---------------------------------------------------------- */

void hearth_os_sleep_ms(uint32_t ms);
void hearth_os_restart(void);

/* ---- bulk working memory (ruling DE419) --------------------------- *
 *
 * One block of `bytes` for a ROW STAGING SESSION, or NULL. The core calls
 * this instead of malloc() so that WHICH heap the block comes from is a
 * platform decision, and that distinction is not academic: on the nRF54L15
 * the application is linked with --wrap=malloc and plain malloc() resolves
 * to the Matter stack's private 10,240-byte working heap
 * (CONFIG_CHIP_MALLOC_SYS_HEAP_OVERRIDE), where a 5,608-byte staging
 * session would take 55% of the memory CHIP needs to run. A hook is the
 * only way core/ can ask for memory without knowing that.
 *
 * What an implementation must satisfy:
 *
 *   - The block must NOT come from memory the SDK's own stack allocates
 *     from, and must not be contended by it. Staging must never be able to
 *     starve commissioning, and commissioning must never be able to fail a
 *     stage. An implementation that satisfies this by picking a pool
 *     nothing else happens to use owes the next reader a note saying so,
 *     and saying what would break it: "nothing else uses it" is a fact
 *     about one link, not a property a build maintains, and no compiler
 *     will notice when it stops being true. hearth_port_zephyr.c carries
 *     the worked example.
 *   - Blocks are a few kilobytes each (one mt_row_stage_t, currently about
 *     5.6 KB) and at most two are live at once.
 *   - Alignment must suit any scalar, int64_t included.
 *   - Callable from more than one task, so the implementation must be
 *     thread-safe. It is never called from an ISR and never with a
 *     hearth_crit_enter() section held, so it MAY block on a lock.
 *   - NULL is a legal answer and the core handles it: AT+MTROW answers a
 *     bare ERROR (AT_MT_SPEC.md section 5's unclassified runtime failure)
 *     and the composition-time commit fails the composition. An
 *     implementation must not panic instead.
 *
 * hearth_stage_free(NULL) is a no-op, like free().
 */
void *hearth_stage_alloc(size_t bytes);
void  hearth_stage_free(void *block);

/* Spawn a detached task. Returns 0 on success, -1 on failure. */
int hearth_os_task_spawn(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, unsigned prio);

typedef void *hearth_sem_t;

/* Binary semaphore, created empty. NULL on allocation failure. */
hearth_sem_t hearth_sem_create_binary(void);
/* timeout_ms 0 polls. True when taken. */
bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms);
void hearth_sem_give(hearth_sem_t sem);

/* ---- link (the AT transport; mutex-serialized like at_uart) ------ */

void hearth_link_init(void);
void hearth_link_write(const void *data, size_t len);
void hearth_link_write_line(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
/*
 * Returns bytes read, 0 on timeout. The AT parser idles in reads of up to
 * 3,600,000 ms (one hour) while the link is silent; implementations must
 * accept and correctly wait out a timeout_ms at least that large, with no
 * overflow in whatever internal unit the wait is converted to and no
 * busy-waiting (a real blocking wait, not a short-timeout poll loop).
 */
int hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
int hearth_link_get_baud(void);
int hearth_link_set_baud(int baud);
int hearth_link_get_flowctrl(void);
int hearth_link_set_flowctrl(int mode);

/* ---- critical sections ------------------------------------------- */

/*
 * Short, non-blocking critical sections, usable from the first
 * instruction (URCs can fire from stack callbacks before any init has
 * run), so the platform owns statically initialized storage and the
 * core names sections by well-known id. Never wrap a blocking call
 * between enter and exit; the cmdbox comments in mt_at.c state the
 * deadlock invariants that depend on this.
 */
#define HEARTH_CRIT_CMDBOX 0
#define HEARTH_CRIT_ROWS   1
#define HEARTH_CRIT_COUNT  2

void hearth_crit_enter(int id);
void hearth_crit_exit(int id);

/* ---- key-value store --------------------------------------------- */

int hearth_kv_get_blob(const char *ns, const char *key,
                       void *buf, size_t *inout_len);
int hearth_kv_set_blob(const char *ns, const char *key,
                       const void *buf, size_t len);
int hearth_kv_get_u8(const char *ns, const char *key, uint8_t *out);
int hearth_kv_set_u8(const char *ns, const char *key, uint8_t val);
/* Delete a key. 0 ok, 1 not-found, -1 error. */
int hearth_kv_delete(const char *ns, const char *key);

#ifdef __cplusplus
}
#endif
