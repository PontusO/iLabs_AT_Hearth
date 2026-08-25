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

/* ---- OS ---------------------------------------------------------- */

void hearth_os_sleep_ms(uint32_t ms);
void hearth_os_restart(void);

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
