/*
 * hearth_port_sl.c - hearth_port.h and hearth_log.h on the Simplicity SDK:
 * FreeRTOS for tasks and semaphores, EUSART0 on PA05/PA06 for the AT link
 * (interrupt-driven RX into hearth_ring, blocking TX), NVM3 for the
 * key-value store, emlib CORE atomic sections. Modelled on
 * platform/nrf54l15/port/hearth_port_zephyr.c; where this file differs the
 * comment says why.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "em_cmu.h"
#include "em_core.h"
#include "em_device.h"
#include "em_eusart.h"
#include "em_gpio.h"
#include "nvm3.h"
#include "nvm3_default.h"

#include "hearth_kvid.h"
#include "hearth_log.h"
#include "hearth_port.h"
#include "hearth_ring.h"
#include "mt_rows.h"    /* mt_row_stage_t; core header, SDK-free */

/* ---- identity ------------------------------------------------------ *
 * The model names the co-processor the host sees (AT+CGMM). Selected on
 * the SoC define, never on a board name, with #error for anything else:
 * a wrong-but-plausible answer is the failure mode the board contract
 * exists to prevent (B429). */
const char *hearth_port_model(void)
{
#if defined(EFR32MG24B310F1536IM48) || defined(EFR32MG24B220F1536IM48) || defined(_EFR32_MG24_FAMILY)
    return "MGM240P Hearth";
#else
#error "hearth_port_model(): unknown Silicon Labs part; add its model string arm"
#endif
}

/* ---- OS ------------------------------------------------------------ */
void hearth_os_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void hearth_os_restart(void)
{
    /* Let the OK that precedes a restart leave the wire first (two
     * character times at 115200 is under 200 us; 5 ms is generous). */
    vTaskDelay(pdMS_TO_TICKS(5));
    NVIC_SystemReset();
}

/* ---- bulk working memory (ruling DE419) ----------------------------- *
 * A dedicated two-slot static pool, not a heap. hearth_port.h asks that
 * staging memory come from somewhere the Matter stack neither allocates
 * from nor contends for, and that "nothing else uses it" be a property the
 * build maintains rather than a fact about one link. A pool nothing else
 * can reach satisfies that by construction: CHIP's allocator, FreeRTOS's
 * heap and libc malloc all stay untouched by this path, and the contract's
 * "at most two live" is the pool's size. NULL when both slots are taken. */
#define STAGE_SLOTS 2
#define STAGE_BYTES 5632   /* >= sizeof(mt_row_stage_t) (5,608 today); int64 aligned */
_Static_assert(sizeof(mt_row_stage_t) <= STAGE_BYTES,
              "grow STAGE_BYTES: mt_row_stage_t no longer fits a staging slot");
static uint8_t s_stage_pool[STAGE_SLOTS][STAGE_BYTES] __attribute__((aligned(8)));
static bool    s_stage_used[STAGE_SLOTS];

/* Atomic (BASEPRI), not critical (PRIMASK): this is a radio SoC, and a
 * critical section masks the RAIL radio interrupt Thread's timing depends
 * on. These sections are a handful of instructions, well within what an
 * atomic section is meant to guard, and RAIL keeps running through them. */

void *hearth_stage_alloc(size_t bytes)
{
    if (bytes > STAGE_BYTES) return NULL;
    void *block = NULL;
    CORE_atomicState_t crit = CORE_EnterAtomic();
    for (int i = 0; i < STAGE_SLOTS; i++) {
        if (!s_stage_used[i]) { s_stage_used[i] = true; block = s_stage_pool[i]; break; }
    }
    CORE_ExitAtomic(crit);
    return block;
}

void hearth_stage_free(void *block)
{
    if (block == NULL) return;
    CORE_atomicState_t crit = CORE_EnterAtomic();
    for (int i = 0; i < STAGE_SLOTS; i++) {
        if (block == s_stage_pool[i]) { s_stage_used[i] = false; break; }
    }
    CORE_ExitAtomic(crit);
}

/* ---- tasks / semaphores -------------------------------------------- */
int hearth_os_task_spawn(const char *name, void (*fn)(void *), void *arg,
                         uint32_t stack_bytes, unsigned prio)
{
    /* prio is deliberately ignored: hearth_port.h defines no priority
     * semantics for a spawned task, and the Zephyr port hardcodes its
     * own priority the same way. */
    (void)prio;
    BaseType_t ok = xTaskCreate(fn, name, stack_bytes / sizeof(StackType_t), arg,
                                tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? 0 : -1;
}

hearth_sem_t hearth_sem_create_binary(void) { return (hearth_sem_t)xSemaphoreCreateBinary(); }

bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void hearth_sem_give(hearth_sem_t sem) { xSemaphoreGive((SemaphoreHandle_t)sem); }

/* ---- critical sections ------------------------------------------------ *
 * CORE_EnterAtomic/CORE_ExitAtomic (BASEPRI), not CORE_EnterCritical
 * (PRIMASK): this is a radio SoC, and HEARTH_CRIT_ROWS in particular wraps
 * row-array work, not two instructions, for long enough that masking
 * PRIMASK would stall the RAIL radio interrupt Thread's timing depends on.
 * Atomic sections need no init, so they are usable from the first
 * instruction, before the scheduler runs, and from an ISR: exactly what
 * the header asks for (URCs can fire from stack callbacks before any
 * init). One saved state per well-known id; the core never nests a
 * section with itself. */
static CORE_atomicState_t s_crit_state[HEARTH_CRIT_COUNT];

void hearth_crit_enter(int id) { s_crit_state[id] = CORE_EnterAtomic(); }
void hearth_crit_exit(int id)  { CORE_ExitAtomic(s_crit_state[id]); }

/* ---- link: EUSART0 on PA05 (TX) / PA06 (RX) ------------------------ *
 * RX is interrupt-driven into a ring: the parser task never touches the
 * peripheral FIFO. The nRF port learned the poll-and-sleep alternative
 * drops bytes ("AT+MTEP=256" lost its '='); the ring is the fix there and
 * the design here. TX is blocking on the shift register under a mutex. */
#define AT_EUSART        EUSART0
#define AT_EUSART_CLK    cmuClock_EUSART0
#define AT_TX_PORT       gpioPortA
#define AT_TX_PIN        5
#define AT_RX_PORT       gpioPortA
#define AT_RX_PIN        6
#define RX_RING_CAP      1024    /* MT_AT_LINE_MAX is 512; headroom is cheap */

static uint8_t           s_rx_storage[RX_RING_CAP];
static hearth_ring_t     s_rx_ring;
static SemaphoreHandle_t s_rx_sem;
static SemaphoreHandle_t s_tx_lock;
static uint32_t          s_rx_dropped_seen;
static int               s_baud = 115200;

void EUSART0_RX_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    /* Clear the pending flag before draining (the standard Silabs
     * ordering), not after: a byte arriving between a last STATUS.RXFL
     * poll and a trailing IntClear would have its own pending flag wiped
     * by that clear and sit in the FIFO until the next byte, stalling a
     * command's terminating newline. Clearing first means a byte that
     * arrives mid-drain re-sets the flag and this handler runs again. */
    EUSART_IntClear(AT_EUSART, EUSART_IF_RXFL);
    while (AT_EUSART->STATUS & EUSART_STATUS_RXFL) {
        (void)hearth_ring_put(&s_rx_ring, (uint8_t)AT_EUSART->RXDATA);
    }
    xSemaphoreGiveFromISR(s_rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void link_configure(int baud)
{
    EUSART_UartInit_TypeDef init = EUSART_UART_INIT_DEFAULT_HF;
    init.baudrate = (uint32_t)baud;
    CMU_ClockEnable(AT_EUSART_CLK, true);
    CMU_ClockEnable(cmuClock_GPIO, true);
    GPIO_PinModeSet(AT_TX_PORT, AT_TX_PIN, gpioModePushPull, 1);
    GPIO_PinModeSet(AT_RX_PORT, AT_RX_PIN, gpioModeInputPull, 1);
    GPIO->EUSARTROUTE[0].TXROUTE = (AT_TX_PORT << _GPIO_EUSART_TXROUTE_PORT_SHIFT)
                                 | (AT_TX_PIN  << _GPIO_EUSART_TXROUTE_PIN_SHIFT);
    GPIO->EUSARTROUTE[0].RXROUTE = (AT_RX_PORT << _GPIO_EUSART_RXROUTE_PORT_SHIFT)
                                 | (AT_RX_PIN  << _GPIO_EUSART_RXROUTE_PIN_SHIFT);
    GPIO->EUSARTROUTE[0].ROUTEEN = GPIO_EUSART_ROUTEEN_TXPEN | GPIO_EUSART_ROUTEEN_RXPEN;
    EUSART_UartInitHf(AT_EUSART, &init);
    EUSART_IntClear(AT_EUSART, _EUSART_IF_MASK);
    EUSART_IntEnable(AT_EUSART, EUSART_IEN_RXFL);
    NVIC_ClearPendingIRQ(EUSART0_RX_IRQn);
    NVIC_EnableIRQ(EUSART0_RX_IRQn);
}

void hearth_link_init(void)
{
    hearth_ring_init(&s_rx_ring, s_rx_storage, RX_RING_CAP);
    s_rx_sem = xSemaphoreCreateBinary();
    s_tx_lock = xSemaphoreCreateMutex();
    link_configure(s_baud);
}

void hearth_link_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    /* hearth_port.h documents that a URC can fire from a stack callback
     * before hearth_link_init() has run, and s_tx_lock is NULL until it
     * does; xSemaphoreTake(NULL, ...) is a hard fault, not a no-op, so
     * write unlocked rather than lock against a mutex that does not
     * exist yet. */
    if (s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) EUSART_Tx(AT_EUSART, p[i]);
    while (!(AT_EUSART->STATUS & EUSART_STATUS_TXC)) {}   /* off the wire */
    if (s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
}

void hearth_link_write_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    /* vsnprintf was given a buffer of sizeof(line) - 2, so on truncation it
     * actually wrote at most sizeof(line) - 3 content bytes before its own
     * NUL; clamping to sizeof(line) - 2 (one too many) would leave that
     * stray NUL sitting in line[] and put it on the wire ahead of the
     * CRLF this appends. */
    if ((size_t)n > sizeof(line) - 3) n = sizeof(line) - 3;
    line[n] = '\r'; line[n + 1] = '\n';
    hearth_link_write(line, (size_t)n + 2);
}

static bool link_wait(uint32_t ms)
{
    /* pdMS_TO_TICKS(3,600,000) at 1 kHz tick is 3,600,000 ticks: within
     * a 32-bit TickType_t, and xSemaphoreTake parks the task, so the
     * one-hour contract is a real blocking wait. */
    return xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static uint32_t link_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

int hearth_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    int got = hearth_ring_read_deadline(&s_rx_ring, buf, (uint32_t)len, timeout_ms,
                                        link_wait, link_now_ms);
    uint32_t dropped = s_rx_ring.dropped;
    if (dropped != s_rx_dropped_seen) {
        HEARTH_LOGW("link", "rx ring overflow, dropped %u byte(s)",
                    (unsigned)(dropped - s_rx_dropped_seen));
        s_rx_dropped_seen = dropped;
    }
    return got;
}

int hearth_link_get_baud(void) { return s_baud; }

int hearth_link_set_baud(int baud)
{
    /* cmd_mtbaud writes its OK at the OLD rate; drain, then switch under
     * the tx lock so nothing queues into the gap (the nRF port's contract). */
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    while (!(AT_EUSART->STATUS & EUSART_STATUS_TXC)) {}
    NVIC_DisableIRQ(EUSART0_RX_IRQn);
    EUSART_BaudrateSet(AT_EUSART, 0, (uint32_t)baud);
    hearth_ring_reset(&s_rx_ring);              /* a byte sampled mid-switch is garbage */
    xQueueReset((QueueHandle_t)s_rx_sem);
    s_baud = baud;
    NVIC_EnableIRQ(EUSART0_RX_IRQn);
    xSemaphoreGive(s_tx_lock);
    return 0;
}

int hearth_link_get_flowctrl(void) { return 0; }
int hearth_link_set_flowctrl(int mode) { return mode == 0 ? 0 : -1; }

/* ---- key-value: NVM3 ---------------------------------------------- *
 * NVM3 keys are 20-bit integers, not paths. Hearth owns the key range
 * 0x0A000..0x0AFFF (CHIP's own keys live elsewhere in the default
 * instance); hearth_kv_id() (hearth_kvid.c, host-tested) hashes (ns, key)
 * to an id inside it. A hash can collide, so every pair the core persists
 * is checked for distinct ids by test_hearth_ring (Task 4 Step 6); a new
 * pair added later must be added there too.
 *
 * nvm3_initDefault() is called lazily, on first use, exactly like the
 * Zephyr port's kv_ensure_init() (hearth_port_zephyr.c): there is no
 * dedicated boot hook for the KV store here, and Task 6's main.c may or
 * may not run the SiSDK's own NVM3 init before mt_at_start(). A failed
 * init does not latch, so a later call retries rather than being wedged
 * for the life of the process. */
static bool s_nvm3_ready;

static int nvm3_ensure_init(void)
{
    if (s_nvm3_ready) return 0;
    if (nvm3_initDefault() != ECODE_NVM3_OK) return -1;
    s_nvm3_ready = true;
    return 0;
}

int hearth_kv_get_blob(const char *ns, const char *key, void *buf, size_t *inout_len)
{
    if (nvm3_ensure_init() != 0) return -1;
    uint32_t type = 0; size_t len = 0;
    Ecode_t rc = nvm3_getObjectInfo(nvm3_defaultHandle, hearth_kv_id(ns, key), &type, &len);
    if (rc == ECODE_NVM3_ERR_KEY_NOT_FOUND) return 1;
    if (rc != ECODE_NVM3_OK || type != NVM3_OBJECTTYPE_DATA) return -1;
    if (len > *inout_len) return -1;
    if (nvm3_readData(nvm3_defaultHandle, hearth_kv_id(ns, key), buf, len) != ECODE_NVM3_OK) return -1;
    *inout_len = len;
    return 0;
}

int hearth_kv_set_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    if (nvm3_ensure_init() != 0) return -1;
    return nvm3_writeData(nvm3_defaultHandle, hearth_kv_id(ns, key), buf, len) == ECODE_NVM3_OK ? 0 : -1;
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
    if (nvm3_ensure_init() != 0) return -1;
    Ecode_t rc = nvm3_deleteObject(nvm3_defaultHandle, hearth_kv_id(ns, key));
    if (rc == ECODE_NVM3_ERR_KEY_NOT_FOUND) return 1;
    return rc == ECODE_NVM3_OK ? 0 : -1;
}

/* ---- log ------------------------------------------------------------- *
 * The console, if one is wired, is a different UART than the AT link
 * (board contract item 6). Until Task 8 decides the console, log lines
 * are dropped rather than written onto the AT link, where they would
 * corrupt URC assertions. */
void hearth_log_write(hearth_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
}
