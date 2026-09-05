# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **skeleton, round 1 in progress** (graph T446). The third Hearth
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

Pending (Task 6): skeleton image size and free RAM at `+MTREADY`.
