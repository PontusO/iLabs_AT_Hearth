/*
 * port_stub.c - hearth_port.h on plain POSIX for the host suite.
 * KV is a RAM table, the link is a no-op, tasks and restart are
 * unreachable from the units under test and abort if hit.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/hearth_log.h"
#include "../../core/include/hearth_port.h"

void hearth_os_sleep_ms(uint32_t ms) { (void)ms; }
void hearth_os_restart(void) { abort(); }
const char *hearth_port_model(void) { return "host-test Hearth"; }

int hearth_os_task_spawn(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, unsigned prio)
{
    (void)name; (void)fn; (void)arg; (void)stack_bytes; (void)prio;
    abort();    /* no unit under test spawns the parser task */
}

hearth_sem_t hearth_sem_create_binary(void) { return malloc(1); }
bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms)
{ (void)sem; (void)timeout_ms; return false; }
void hearth_sem_give(hearth_sem_t sem) { (void)sem; }

void hearth_crit_enter(int id) { (void)id; }
void hearth_crit_exit(int id)  { (void)id; }

/* Row staging memory (ruling DE419). Plain malloc is the right backing on
 * the host, where there is no second heap to get wrong; the two firmware
 * ports differ, and hearth_port.h says why. */
void *hearth_stage_alloc(size_t bytes) { return malloc(bytes); }
void  hearth_stage_free(void *block)   { free(block); }

void hearth_link_init(void) {}
void hearth_link_write(const void *data, size_t len) { (void)data; (void)len; }
void hearth_link_write_line(const char *fmt, ...) { (void)fmt; }
int  hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{ (void)buf; (void)len; (void)timeout_ms; return 0; }
int  hearth_link_get_baud(void) { return 115200; }
int  hearth_link_set_baud(int baud) { (void)baud; return 0; }
int  hearth_link_get_flowctrl(void) { return 0; }
int  hearth_link_set_flowctrl(int mode) { (void)mode; return 0; }

/* ---- RAM KV ------------------------------------------------------ */

#define STUB_KV_MAX 32

static struct {
    char    ns[16], key[16];
    uint8_t blob[512];
    size_t  len;
    int     used;
} s_kv[STUB_KV_MAX];

static int kv_find(const char *ns, const char *key)
{
    for (int i = 0; i < STUB_KV_MAX; i++)
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
        for (int j = 0; j < STUB_KV_MAX; j++)
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
    (void)level; (void)tag; (void)fmt;
}
