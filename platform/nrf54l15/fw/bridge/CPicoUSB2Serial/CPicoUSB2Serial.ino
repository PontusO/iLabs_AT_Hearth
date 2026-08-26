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

#define PIN_UART_TX   0   /* CPico GP0  -> module P1.15 (UARTE20 RX) */
#define PIN_UART_RX   1   /* CPico GP1  <- module P1.04 (UARTE20 TX) */
#define PIN_NRESET    2   /* CPico GP2  -> module nRESET pad          */
#define PIN_STRAP     3   /* CPico GP3  -> module P2.03               */

static void drive_low_or_release(int pin, bool asserted)
{
    if (asserted) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    } else {
        pinMode(pin, INPUT);   /* released: module pull-up wins */
    }
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

    /* Follow the host's baud selection. */
    uint32_t baud = Serial.baud();
    if (baud != last_baud && baud != 0) {
        Serial1.end();
        Serial1.begin(baud);
        last_baud = baud;
    }

    /* Control lines follow CDC line state every pass. */
    drive_low_or_release(PIN_NRESET, Serial.dtr());
    drive_low_or_release(PIN_STRAP, Serial.rts());

    while (Serial.available())  Serial1.write(Serial.read());
    while (Serial1.available()) Serial.write(Serial1.read());
}
