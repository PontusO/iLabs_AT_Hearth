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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "em_cmu.h"
#include "em_core.h"
#include "em_device.h"
#include "em_eusart.h"
#include "em_gpio.h"
#include "em_usart.h"
#include "nvm3.h"
#include "nvm3_default.h"

#include "hearth_console.h"
#include "hearth_kvid.h"
#include "hearth_log.h"
#include "hearth_port.h"
#include "hearth_ring.h"
#include "mt_rows.h"    /* mt_row_stage_t; core header, SDK-free */

/* ---- identity ------------------------------------------------------ *
 * The model names the co-processor the host sees (AT+CGMM). Selected on
 * the part define the SiSDK project carries, never on a board name, with
 * #error for anything else: a wrong-but-plausible answer is the failure
 * mode the board contract exists to prevent (B429).
 *
 * MGM240PA32VNA is the module id, measured in Task 5 (README, "Do not
 * build for BRD2704A"). Naming it alone, rather than an
 * _EFR32_MG24_FAMILY catch-all, turns the BRD2704A trap into a compile
 * error: that board's project defines MGM240PB32VNA, the B part, whose
 * image RAIL-asserts on this module at runtime and is silent on every
 * UART. A build that cannot start is cheaper than one that dies in the
 * radio init. A future EFR32MG24 board adds its own arm here. */
const char *hearth_port_model(void)
{
#if defined(MGM240PA32VNA)
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

/* ---- bulk working memory (rulings DE419 and DE624) ------------------ *
 * Two tenants with opposite shapes. Row staging (mt_row_stage_t, 5,608 B)
 * gets a dedicated two-slot static pool, not a heap: hearth_port.h asks
 * that staging memory come from somewhere the Matter stack neither
 * allocates from nor contends for, and a pool nothing else can reach
 * satisfies that by construction; the contract's "at most two live" is the
 * pool's size.
 *
 * The firmware-over-the-air relay's one block (1,024 B, held for a whole
 * download) comes from the heap instead, sl_memory_manager's, which is
 * also CHIP's (DE624, the user, 2026-09-28). The pool cannot hold it: an
 * EVSE's inbound row stage keeps one slot for the whole boot and AT+MTROW
 * takes the other, so a download would fail or AT+MTROW would. The heap
 * costs nothing while no update runs (the FOTA spec's section 8.4), and
 * 1 kB against about 63 kB free at +MTREADY cannot starve commissioning.
 * This is the one stated exception to hearth_port.h's first rule, and the
 * size is what tells the two tenants apart: every row stage is larger
 * than STAGE_SMALL_MAX, which the assert below keeps true. */
#define STAGE_SLOTS 2
#define STAGE_BYTES 5632   /* >= sizeof(mt_row_stage_t) (5,608 today); int64 aligned */
#define STAGE_SMALL_MAX 1024
_Static_assert(sizeof(mt_row_stage_t) <= STAGE_BYTES,
              "grow STAGE_BYTES: mt_row_stage_t no longer fits a staging slot");
_Static_assert(sizeof(mt_row_stage_t) > STAGE_SMALL_MAX,
              "a row stage would now be served from the heap: revisit DE624");
static uint8_t s_stage_pool[STAGE_SLOTS][STAGE_BYTES] __attribute__((aligned(8)));
static bool    s_stage_used[STAGE_SLOTS];

/* Atomic (BASEPRI), not critical (PRIMASK): this is a radio SoC, and a
 * critical section masks the RAIL radio interrupt Thread's timing depends
 * on. These sections are a handful of instructions, well within what an
 * atomic section is meant to guard, and RAIL keeps running through them. */

void *hearth_stage_alloc(size_t bytes)
{
    if (bytes <= STAGE_SMALL_MAX) {
        void *small = malloc(bytes);
        /* hearth_port.h promises int64_t alignment; refuse rather than
         * hand out a block that breaks it. */
        if (small != NULL && ((uintptr_t)small & 7u) != 0) {
            free(small);
            return NULL;
        }
        return small;
    }
    if (bytes > STAGE_BYTES) return NULL;
    void *block = NULL;
    CORE_irqState_t crit = CORE_EnterAtomic();
    for (int i = 0; i < STAGE_SLOTS; i++) {
        if (!s_stage_used[i]) { s_stage_used[i] = true; block = s_stage_pool[i]; break; }
    }
    CORE_ExitAtomic(crit);
    return block;
}

void hearth_stage_free(void *block)
{
    if (block == NULL) return;
    bool pooled = false;
    CORE_irqState_t crit = CORE_EnterAtomic();
    for (int i = 0; i < STAGE_SLOTS; i++) {
        if (block == s_stage_pool[i]) { s_stage_used[i] = false; pooled = true; break; }
    }
    CORE_ExitAtomic(crit);
    if (!pooled) free(block);   /* outside the atomic section: free() may lock */
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

/* ---- monotonic ms clock ---------------------------------------------- *
 * xTaskGetTickCount() * portTICK_PERIOD_MS is 0 forever once
 * configTICK_RATE_HZ exceeds 1000 Hz: portTICK_PERIOD_MS is the integer
 * 1000 / configTICK_RATE_HZ, which truncates to 0 above 1000 Hz, and
 * Silabs configs commonly tick at 1024 Hz. Multiply before dividing
 * instead, in 64 bits so the intermediate product cannot overflow across
 * the full 32-bit tick range at any realistic tick rate. Shared by
 * hearth_sem_take's timeout slicing and the link's deadline wait. */
static uint32_t hearth_now_ms(void)
{
    return (uint32_t)((uint64_t)xTaskGetTickCount() * 1000u / configTICK_RATE_HZ);
}

/* pdMS_TO_TICKS(ms) computes ms * configTICK_RATE_HZ / 1000 in whatever
 * width TickType_t is; for a 32-bit TickType_t that multiply can overflow
 * before the divide runs once ms is large and configTICK_RATE_HZ is above
 * roughly 1193 Hz (ms near the header's one-hour ceiling is exactly the
 * case that matters here). Waiting in bounded slices and re-deriving the
 * remaining time from hearth_now_ms() avoids ever handing pdMS_TO_TICKS a
 * value that can overflow, regardless of the project's tick rate. */
#define HEARTH_WAIT_SLICE_MS 60000u

hearth_sem_t hearth_sem_create_binary(void) { return (hearth_sem_t)xSemaphoreCreateBinary(); }

bool hearth_sem_take(hearth_sem_t sem, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return xSemaphoreTake((SemaphoreHandle_t)sem, 0) == pdTRUE;
    }
    uint32_t start = hearth_now_ms();
    for (;;) {
        uint32_t elapsed = hearth_now_ms() - start;   /* wraps correctly if it ever does */
        if (elapsed >= timeout_ms) return false;
        uint32_t remain = timeout_ms - elapsed;
        uint32_t slice = remain > HEARTH_WAIT_SLICE_MS ? HEARTH_WAIT_SLICE_MS : remain;
        if (xSemaphoreTake((SemaphoreHandle_t)sem, pdMS_TO_TICKS(slice)) == pdTRUE) return true;
    }
}

void hearth_sem_give(hearth_sem_t sem) { xSemaphoreGive((SemaphoreHandle_t)sem); }

/* ---- critical sections ------------------------------------------------ *
 * CORE_EnterAtomic/CORE_ExitAtomic (BASEPRI), not CORE_EnterCritical
 * (PRIMASK): this is a radio SoC, and HEARTH_CRIT_ROWS in particular wraps
 * row-array work, not two instructions, for long enough that masking
 * PRIMASK would stall the RAIL radio interrupt Thread's timing depends on.
 * An atomic section excludes every interrupt whose NVIC priority is at or
 * below (numerically at or above) CORE_ATOMIC_BASE_PRIORITY_LEVEL; it does
 * NOT exclude an interrupt configured above that level (a lower number,
 * i.e. more urgent), which is exactly why link_configure() sets
 * EUSART0_RX_IRQn's priority down to CORE_ATOMIC_BASE_PRIORITY_LEVEL
 * before enabling it: with that done, this section's guarantee does cover
 * the RX ISR too. Atomic sections need no init, so they are usable from
 * the first instruction, before the scheduler runs, and from an ISR:
 * exactly what the header asks for (URCs can fire from stack callbacks
 * before any init). One saved state per well-known id, which needs the
 * stronger invariant than "no id nests with itself": NO section may be open
 * while another is entered, whatever the ids. Two different ids nested
 * would each save a BASEPRI, and the inner exit would restore the outer's
 * pre-section value, reopening the window early; the outer exit would then
 * restore a value captured inside a section. The final whole-branch review
 * walked every hearth_crit_enter/exit pair in core/mt/mt_at.c on
 * 2026-09-17 and found no nesting of any kind. */
static CORE_irqState_t s_crit_state[HEARTH_CRIT_COUNT];

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
/* The link's mirror of s_console_ready (declared with the console's own
 * statics further down, where its peripheral is): false until EUSART0 is
 * clocked and configured. hearth_link_write() reads it. */
static volatile bool     s_link_ready;

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
    /* Cortex-M IPR resets to 0 (the highest possible priority), which sits
     * above configMAX_SYSCALL_INTERRUPT_PRIORITY: a handler running there
     * cannot safely call an ISR-safe FreeRTOS API (xSemaphoreGiveFromISR
     * below) since vPortValidateInterruptPriority asserts on that call
     * with asserts compiled in, or corrupts the ready lists with them
     * compiled out. It also means CORE_EnterAtomic's BASEPRI mask would
     * not exclude this ISR, breaking hearth_crit_enter/exit's guarantee
     * for HEARTH_CRIT_ROWS. Lower the priority to the atomic base level
     * before enabling the IRQ so both hold. */
    NVIC_SetPriority(EUSART0_RX_IRQn, CORE_ATOMIC_BASE_PRIORITY_LEVEL);
    NVIC_ClearPendingIRQ(EUSART0_RX_IRQn);
    NVIC_EnableIRQ(EUSART0_RX_IRQn);
}

void hearth_link_init(void)
{
    /* RX_RING_CAP is a compile-time power-of-two constant, so this check
     * is unreachable in practice; it is here so a future edit to that
     * constant fails loudly at runtime instead of silently corrupting the
     * ring's index arithmetic. */
    if (hearth_ring_init(&s_rx_ring, s_rx_storage, RX_RING_CAP) != 0) {
        HEARTH_LOGE("link", "hearth_ring_init rejected RX_RING_CAP=%u", (unsigned)RX_RING_CAP);
        return;
    }
    s_rx_sem = xSemaphoreCreateBinary();
    s_tx_lock = xSemaphoreCreateMutex();
    link_configure(s_baud);
    s_link_ready = true;
}

void hearth_link_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    /* A URC from a stack callback can arrive before hearth_link_init()
     * (hearth_port.h says so). Before init EUSART0 is unclocked and
     * disabled, and EUSART_Tx on it hangs on TXFL or faults on the
     * register read; the mutex guard below covered only the lock half.
     * Drop the write: the boot contract drops pre-+MTREADY output anyway. */
    if (!s_link_ready) return;
    /* The peripheral guard is s_link_ready, set at the end of
     * hearth_link_init(); the mutex guard is the s_tx_lock NULL check
     * below, because xSemaphoreTake(NULL, ...) faults rather than no-ops. */
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
    /* xSemaphoreTake parks the task, so this is a real blocking wait, not
     * a poll; clamped to HEARTH_WAIT_SLICE_MS for the same overflow reason
     * as hearth_sem_take (see hearth_now_ms's comment). hearth_ring_read_
     * deadline recomputes the true remaining time and calls this again if
     * a slice times out before the real deadline, so slicing here is
     * correct, not just safe, for the header's one-hour ceiling. */
    uint32_t slice = ms > HEARTH_WAIT_SLICE_MS ? HEARTH_WAIT_SLICE_MS : ms;
    return xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(slice)) == pdTRUE;
}

static uint32_t link_now_ms(void)
{
    return hearth_now_ms();
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
     * the tx lock so nothing queues into the gap (the nRF port's
     * contract). Guarded against s_tx_lock == NULL the same way
     * hearth_link_write's mutex half is: AT+MTBAUD is a command reply, not
     * a stack callback, so in practice hearth_link_init() has always run by
     * the time this is reachable, but the guard is one line and keeps both
     * paths consistent rather than relying on that distinction. No
     * s_link_ready check here for the same reason: this one is reached only
     * from the parser task, which exists only after mt_at_start(). */
    if (s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    while (!(AT_EUSART->STATUS & EUSART_STATUS_TXC)) {}
    NVIC_DisableIRQ(EUSART0_RX_IRQn);
    EUSART_BaudrateSet(AT_EUSART, 0, (uint32_t)baud);
    /* A byte that landed between NVIC_DisableIRQ and here was received at
     * the OLD rate, is not yet drained (the ISR that would drain it is
     * disabled), and would otherwise sit in the FIFO to be read back at
     * the NEW rate as garbage; discard it before anything downstream can
     * see it. */
    while (AT_EUSART->STATUS & EUSART_STATUS_RXFL) (void)AT_EUSART->RXDATA;
    hearth_ring_reset(&s_rx_ring);              /* a byte sampled mid-switch is garbage */
    xQueueReset((QueueHandle_t)s_rx_sem);
    s_baud = baud;
    NVIC_EnableIRQ(EUSART0_RX_IRQn);
    if (s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
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
 * The console is a different UART than the AT link (board contract item
 * 6), and it is USART0 TX on PA00 (module pin 7), 115200 8N1, the only
 * console line the carrier wires to the Debug Probe's UART CDC. Ruling
 * 2026-09-17 (design spec section 7 addendum) moved this forward from
 * Task 8, because Task 5 could never establish that the probe's RX
 * really reaches PA00: the stock example was silent there, and silence
 * on an unproven path proves nothing. Hearth's own boot log is the
 * traffic that settles it.
 *
 * TX only. PA03 (module pin 10) is the header's RX line and is
 * deliberately left uninitialised: a shipping image has no console input
 * path (CRA_COMPLIANCE.md, the CHIP shell row). PA01/PA02 are SWD and
 * are never handed to a UART (the Task 5 trap, README).
 *
 * Bare USART_InitAsync/USART_Tx rather than the SDK's sl_iostream_usart
 * component: iostream would add an instance whose generated config
 * defaults the RX pin to PA01, which is SWCLK, and the whole value of it
 * here (RX, buffering, stdio retarget) is value this port must not have.
 * Sixty lines of emlib have no defaults to get wrong.
 *
 * s_log_lock is the console's own mutex, NOT the AT link's s_tx_lock.
 * FreeRTOS mutexes do not recurse, so sharing one would deadlock the
 * first time a log line is ever emitted from inside a hearth_link_write
 * that holds it, and a console line must not queue behind a long AT
 * response either. Log lines never touch EUSART0. */
#define LOG_USART      USART0
#define LOG_USART_CLK  cmuClock_USART0
#define LOG_TX_PORT    gpioPortA
#define LOG_TX_PIN     0

static SemaphoreHandle_t s_log_lock;
static volatile bool     s_console_ready;

/* Called once from app_init_early() (src/main.cpp), which sl_main runs
 * after the clock manager and before the kernel. Declared in
 * port/hearth_console.h, so check_decls.py sees it. Logs raised before it
 * are dropped: the peripheral is not configured yet, and s_console_ready
 * is the gate that says so. */
void hearth_console_init(void)
{
    USART_InitAsync_TypeDef init = USART_INITASYNC_DEFAULT;
    init.enable   = usartEnableTx;      /* TX only; see above */
    init.baudrate = 115200;

    CMU_ClockEnable(cmuClock_GPIO, true);
    CMU_ClockEnable(LOG_USART_CLK, true);
    GPIO_PinModeSet(LOG_TX_PORT, LOG_TX_PIN, gpioModePushPull, 1);
    GPIO->USARTROUTE[0].TXROUTE = (LOG_TX_PORT << _GPIO_USART_TXROUTE_PORT_SHIFT)
                                | (LOG_TX_PIN  << _GPIO_USART_TXROUTE_PIN_SHIFT);
    GPIO->USARTROUTE[0].ROUTEEN = GPIO_USART_ROUTEEN_TXPEN;
    USART_InitAsync(LOG_USART, &init);

    s_log_lock = xSemaphoreCreateMutex();
    s_console_ready = true;
}

/* Put one already-formatted line on the wire with no mutex and no yield.
 * Blocking on TXC is a spin, not a scheduler call, so it is legal anywhere. */
static void console_write_raw(const char *line, int len)
{
    for (int i = 0; i < len; i++) USART_Tx(LOG_USART, (uint8_t)line[i]);
    while (!(LOG_USART->STATUS & USART_STATUS_TXC)) {}
}

/* May this call wait on s_log_lock, or must it write straight at the
 * peripheral?
 *
 * THIS IS THE FAULT PATH'S QUESTION, and getting it wrong is the silent hang
 * that strong fault hooks exist to prevent. src/sdk/SoftwareFaultReports.cpp
 * emits ChipLogError from three places that are not ordinary task context, and
 * CHIP's log redirect (port/hearth_matter_init.cpp) brings every one of them
 * here:
 *
 *   - debugHardfault(), reached from HardFault_Handler, BusFault_Handler,
 *     UsageFault_Handler, mpu_fault_handler, SecureFault_Handler,
 *     DebugMon_Handler and WDOG0_IRQHandler: handler mode.
 *   - vApplicationStackOverflowHook(), called from vTaskSwitchContext() inside
 *     the PendSV handler: handler mode, and inside a context switch.
 *   - RAILCb_AssertFailed(), called from the radio interrupt: handler mode.
 *
 * A FreeRTOS mutex must not be touched from an interrupt AT ALL, with any
 * timeout: xSemaphoreTakeFromISR() is documented as unusable with mutexes, and
 * xSemaphoreTake(x, 0) enters a taskENTER_CRITICAL() section that is illegal
 * from an ISR on Cortex-M because its exit unconditionally unmasks. So the
 * answer in handler mode is not a shorter wait, it is no mutex.
 *
 * Measured on the bench, 2026-09-18, with a scratch probe that took s_log_lock
 * and then wrote to 0xFFFFFFF0: before this guard the console stopped dead at
 * the probe's own line and the whole fault report was lost; after it, the
 * report is complete. The mutex being FREE is what made the unguarded version
 * look like it worked, which is the worst way for this to be wrong.
 *
 * The scheduler-state test covers the other half: app_init_early() logs before
 * the kernel exists, and a suspended scheduler is not a place to block either.
 *
 * The cost of the lock-free path is interleaving: a fault report can cut into a
 * line another task is mid-way through writing. Interleaved output beats no
 * output. */
static bool log_may_block(void)
{
    if (s_log_lock == NULL) return false;
    if (__get_IPSR() != 0U) return false;                        /* handler mode */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return false;
    return true;
}

void hearth_log_write(hearth_log_level_t level, const char *tag, const char *fmt, ...)
{
    static const char lvl[] = { 'E', 'W', 'I' };
    char line[192];
    va_list ap;

    if (!s_console_ready) return;

    int n = snprintf(line, sizeof(line) - 2, "%c %s: ",
                     level <= HEARTH_LOG_INFO ? lvl[level] : '?', tag);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 3) n = (int)sizeof(line) - 3;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - 2 - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    /* Same clamp as hearth_link_write_line: vsnprintf was given two bytes
     * less than the buffer, so on truncation it wrote at most
     * sizeof(line) - 3 content bytes before its own NUL, and clamping one
     * higher would put that NUL on the wire ahead of the CRLF. */
    if ((size_t)n > sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n] = '\r'; line[n + 1] = '\n';

    if (!log_may_block()) {
        console_write_raw(line, n + 2);
        return;
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    console_write_raw(line, n + 2);
    xSemaphoreGive(s_log_lock);
}
