# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **round 1 software half complete; Tasks 5 to 8 pending the
MGM240PA32VNA3** (graph T446). The third Hearth
platform, mimicking the nRF54L15 port: a Thread FTD + BLE co-processor
serving the `AT+MT` contract over one UART. Design:
`iLabs_Hearth_docs/superpowers/specs/2026-09-05-silabs-mg24-port-design.md`.

This README is the platform bible, in the shape of `platform/nrf54l15/README.md`:
the board contract, the toolchain, flashing, and the measured figures are
recorded here as each lands. Sections marked "pending" are filled by the
task that produces them.

## Board: the iLabs RP2350 carrier with an MGM240PA32VNA3

| Line | Module pin | Signal | Note |
|---|---|---|---|
| AT UART TX | PA05 (pin 12) | EUSART0 TX | the factory UART XMODEM bootloader's TX |
| AT UART RX | PA06 (pin 13) | EUSART0 RX | the factory bootloader's RX |
| Reset | RESETn (pin 31) | active low | internal pull-up; drive low only, never high |
| SWD | PA01 (pin 8), PA02 (pin 9) | SWCLK, SWDIO | one-time bootloader work, debug |
| Recovery strap | pending (Task 7) | GPIO activation | confirm against the factory bootloader's BTL_BUTTON |
| Power | VDD (pin 15) | 3.3 V nominal | +20 dBm part; ~160 mA TX peaks |

The Ezurio Lyra 24P this carrier was laid out for is an EFR32BG24 (Bluetooth
only) and shares this exact footprint; the MGM240P is the drop-in.

## Toolchain

Pending (Task 5): Simplicity SDK 2025.12, Silicon Labs Matter 2.8.0-1.5.

## Flashing

Pending (Task 7): `fw/flash.py` over the factory UART XMODEM bootloader.

## Measured

Pending (Task 6): skeleton image size and free RAM at `+MTREADY`. Note for
whoever fills this in: `hearth_log_write` (`hearth_port_sl.c`) discards
everything it is given until a console UART is wired (Task 8), so the RX
ring overflow warning it would otherwise print is unobservable until then;
an absence of overflow warnings in this section is not evidence the ring
never overflowed.

## First-compile checklist (Task 6)

`hearth_port_sl.c` (Task 4) was written and host-tested for its pure-C
pieces without a Simplicity SDK toolchain, so several SDK identifier names
and behaviors are unverified. Task 6, the first real compile, must check:

- `GPIO->EUSARTROUTE[0]` field names (`TXROUTE`/`RXROUTE`/`ROUTEEN`) and the
  `_GPIO_EUSART_*ROUTE_PORT_SHIFT` / `_PIN_SHIFT` macros against the real
  EFR32MG24 device header; `EUSART0_RX_IRQn` likewise.
- `EUSART_UART_INIT_DEFAULT_HF`'s default RX FIFO watermark: `hearth_ring`'s
  RX ISR assumes `EUSART_STATUS_RXFL`/`EUSART_IF_RXFL` fire per received
  byte (the nRF port's bench-proven bug, "AT+MTEP=256" losing its '=', is
  exactly what a coarser watermark would reintroduce here).
- The default CMU clock source EUSART0 is fed from when
  `CMU_ClockEnable(cmuClock_EUSART0, true)` is called with no prior
  `CMU_ClockSelectSet`, and whether that gives 115200 baud without drift.
- `configUSE_MUTEXES` is enabled in `FreeRTOSConfig.h`: `hearth_port_sl.c`
  calls `xSemaphoreCreateMutex()` for the TX lock.
- `CORE_atomicState_t`, `CORE_EnterAtomic()` and `CORE_ExitAtomic()` are the
  actual emlib `em_core.h` API for BASEPRI-based atomic sections (as
  opposed to, for example, a single shared `CORE_irqState_t` type or a
  macro-only interface); `hearth_port_sl.c` uses the function-pair form
  named in the Task 4 fix-round-1 review.
- The Hearth-owned NVM3 key range `0x0A000..0x0AFFF` (`hearth_kvid.h`,
  `HEARTH_KV_BASE`/`HEARTH_KV_SPAN`) does not collide with CHIP's own
  `SilabsConfig` NVM3 keys or the OpenThread EFR32 settings backend's NVM3
  keys in the default NVM3 instance.
- The `_EFR32_MG24_FAMILY` fallback arm in `hearth_port_model()`'s `#if`
  (`hearth_port_sl.c`) is replaced with the exact part define the SiSDK
  project sets for the MGM240PA32VNA3, once this README records it under
  Toolchain/Board.
- The RX IRQ's priority (`NVIC_SetPriority(EUSART0_RX_IRQn,
  CORE_ATOMIC_BASE_PRIORITY_LEVEL)`, `hearth_port_sl.c`, `link_configure`)
  must be numerically at or below `configMAX_SYSCALL_INTERRUPT_PRIORITY`;
  confirm `CORE_ATOMIC_BASE_PRIORITY_LEVEL` actually resolves to a value
  that satisfies that for this project's `FreeRTOSConfig.h`, not just that
  the macro exists.
- `configTICK_RATE_HZ`: confirm its actual value (commonly 1024 Hz on
  Silabs configs, not 1000), and that the header's one-hour timeout ceiling
  does not overflow `TickType_t` at that rate after the fix that made
  `hearth_now_ms()` and `hearth_sem_take`/`link_wait`'s wait-slicing
  (`hearth_port_sl.c`) tick-rate-independent.
- `nvm3_initDefault()` is called lazily by this port
  (`nvm3_ensure_init()`, `hearth_port_sl.c`); `sl_system_init()` already
  initialises NVM3 when the project's `nvm3_default` component is present.
  Confirm the port's second call is a harmless no-op returning
  `ECODE_NVM3_OK` in that case (SiSDK's own doc for `nvm3_initDefault()`
  should say), or drop the lazy init here if the project guarantees the
  component is always present.
- `EUSART_BaudrateSet()` (`hearth_link_set_baud`, `hearth_port_sl.c`) is
  called with the EUSART left enabled, mid-session, not disabled first.
  Confirm CLKDIV accepts a write in that state on this part, or add a
  disable/enable cycle around the call if it does not.
