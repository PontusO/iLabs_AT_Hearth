/*
 * CPicoUSB2Serial - USB CDC to Ophelia-IV AT UART bridge with DFU
 * control lines, for the CPico + Ophelia-IV dev board.
 *
 * Mapping (the flasher's contract):
 *   CDC DTR asserted -> module nRESET driven low (held in reset)
 *   CDC RTS asserted -> recovery strap driven low (active low, has an
 *                       on-module pull-up; MCUboot samples it at boot)
 *   line released    -> pin released to input (high-Z), module runs
 *
 * A terminal that asserts neither line talks to the running
 * application. The console is NOT forwarded here: it lives on the
 * module's second UART, watched via the Debug Probe.
 */

#include "tusb.h"

#define PIN_UART_TX   0   /* CPico GP0  -> module P1.15 (UARTE20 RX) */
#define PIN_UART_RX   1   /* CPico GP1  <- module P1.04 (UARTE20 TX) */
#define PIN_NRESET    2   /* CPico GP2  -> module nRESET pad          */
#define PIN_STRAP     3   /* CPico GP3  -> module P2.03               */

static volatile uint32_t pending_baud = 0;   /* set by CDC callback, applied in loop() */

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    pending_baud = coding->bit_rate;
}

static volatile bool line_dtr = false;
static volatile bool line_rts = false;
static volatile bool line_evt = false;

/* TinyUSB weak-callback override: cache CDC line state for the loop. */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    line_dtr = dtr;
    line_rts = rts;
    line_evt = true;
}

/* Per-pin cache of the last state actually applied. pinMode() in
 * arduino-pico calls gpio_init(), which momentarily releases the pad
 * before reconfiguring it; calling drive_low_or_release() again with
 * the state it is already in would glitch an already-driven pin.
 * Caching lets the function return early when nothing changed. */
struct pin_cache {
    bool applied;
    bool valid;
};

static struct pin_cache nreset_cache = { false, false };
static struct pin_cache strap_cache  = { false, false };

/* struct pin_cache (elaborated, not the typedef) in the signature below
 * is deliberate: arduino-cli's auto-generated prototype for this
 * function is inserted above every other declaration in the sketch,
 * before a typedef here would be visible, and "struct pin_cache" alone
 * needs no prior typedef to parse. */
static void drive_low_or_release(int pin, bool asserted, struct pin_cache *cache)
{
    if (cache->valid && cache->applied == asserted) {
        return;   /* already in this state: do not re-touch the pad */
    }
    if (asserted) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    } else {
        pinMode(pin, INPUT);   /* released: module pull-up wins */
    }
    cache->applied = asserted;
    cache->valid = true;
}

void setup()
{
    pinMode(PIN_NRESET, INPUT);
    pinMode(PIN_STRAP, INPUT);
    Serial.begin(115200);
    Serial1.setTX(PIN_UART_TX);
    Serial1.setRX(PIN_UART_RX);
    Serial1.begin(115200);
}

void loop()
{
    static uint32_t last_baud = 115200;

    /* Apply baud change from CDC line-coding callback, out here where UART
       teardown and rebuild cannot deadlock the USB stack. */
    uint32_t new_baud;
    noInterrupts();
    new_baud = pending_baud;
    pending_baud = 0;
    interrupts();
    if (new_baud && new_baud != last_baud) {
        Serial1.end();
        Serial1.begin(new_baud);
        last_baud = new_baud;
    }

    /* A USB unplug while DTR/RTS are asserted must not leave the module
     * held in reset forever: TinyUSB fires no line-state callback on a
     * disconnect or unmount, so line_dtr/line_rts stay latched at
     * whatever they were the instant the cable dropped. Detect the
     * unmount here and synthesize the release ourselves. This is the
     * fix for the "unplug the bridge mid-session and the module never
     * comes back without a manual power cycle" regression. */
    if (!tud_mounted() && (line_dtr || line_rts)) {
        line_dtr = false;
        line_rts = false;
        line_evt = true;
    }

    /* Control lines are applied on line-state EVENTS only (the C6
     * bridge pattern). The real defect this avoids: pinMode() in
     * arduino-pico calls gpio_init(), which momentarily releases the
     * pad before reconfiguring it, so re-sampling Serial.dtr()/rts()
     * and re-driving pinMode every loop pass glitched the strap on
     * EVERY pass, unconditionally, not just when a read happened to
     * flicker. MCUboot's continuous-strap debounce window is
     * CONFIG_BOOT_SERIAL_DETECT_DELAY=50 ms, and that per-pass glitch
     * broke it roughly two times in three on the bench. The callback
     * above caches the state; this block applies it once per change,
     * and drive_low_or_release() is itself idempotent now, so even a
     * repeated identical event cannot re-glitch the pad. STRAP is
     * applied before nRESET so the strap is already settled before the
     * reset line moves, in either direction. */
    if (line_evt) {
        noInterrupts();
        bool dtr = line_dtr;
        bool rts = line_rts;
        line_evt = false;
        interrupts();
        drive_low_or_release(PIN_STRAP, rts, &strap_cache);
        drive_low_or_release(PIN_NRESET, dtr, &nreset_cache);
    }

    while (Serial.available())  Serial1.write(Serial.read());

    /* Module-to-host bytes go through TinyUSB directly, NOT through
     * Serial.write(): the arduino-pico CDC discards TX while DTR is
     * low, and this bridge's contract REQUIRES the host to keep DTR
     * low in normal operation (DTR asserted holds the module in
     * reset). Found on the bench: with Serial.write() the module's
     * +MTREADY never reached the host. */
    while (Serial1.available()) {
        uint8_t b = (uint8_t)Serial1.read();
        tud_cdc_write(&b, 1);
    }
    /* tud_cdc_write() auto-flushes on its own once it fills a full USB
     * packet; this call is only the tail flush for whatever is left in
     * the FIFO below a full packet. */
    tud_cdc_write_flush();
}
