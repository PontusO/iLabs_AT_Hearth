# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **round 1 software half complete; the toolchain, the project's own
bootloader and a stock Silabs Matter-over-Thread example are bench-proven on
the MGM240PA32VNA3 (Task 5, 2026-09-17); Tasks 6 to 8 pending** (graph T446).
The third Hearth
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
| AT UART TX | PA05 (pin 12) | EUSART0 TX (app), USART0 TX (bootloader) | the bootloader's TX |
| AT UART RX | PA06 (pin 13) | EUSART0 RX (app), USART0 RX (bootloader) | the bootloader's RX |
| Console | PA00 (pin 7) | USART0 TX | wired to the Debug Probe's UART CDC; nothing observed on it yet (see Toolchain) |
| Reset | RESETn (pin 31) | active low | internal pull-up; drive low only, never high |
| SWD | PA01 (pin 8), PA02 (pin 9) | SWCLK, SWDIO | bootloader install, debug |
| Recovery strap | PC00 (pin 22) | `SL_BTL_BUTTON`, active low | held low through reset enters the bootloader; proven both ways 2026-09-17 |
| Power | VDD (pin 15) | 3.3 V nominal | +20 dBm part; ~160 mA TX peaks |

Silicon, measured over SWD 2026-09-17 (`openocd -f mg24.cfg -c "init; flash
banks; flash info 0"`): **EFR32MG24 A620, rev 7**, flash 1536 KiB, page 8192 B.
The SE reports debug lock disabled, device erase enabled, secure debug
disabled; none of that is changed by this port (signing, secure boot and the
debug lock stay a pre-ship step).

The SDK part id for this module is **`MGM240PA32VNA`** (the trailing `3` of the
marking is a packaging suffix and is not part of the id), and the compiler
define a project built for it carries is `-DMGM240PA32VNA=1`.

The Ezurio Lyra 24P this carrier was laid out for is an EFR32BG24 (Bluetooth
only) and shares this exact footprint; the MGM240P is the drop-in.

### Do not build for BRD2704A

BRD2704A (SparkFun Thing Plus MGM240P) is the one Silabs-supported board with
an MGM240P on it, and its board files describe this carrier's pin map exactly:
bootloader UART on PA05/PA06 at 115200, `SL_BTL_BUTTON` on port C pin 0. It is
still the wrong target. BRD2704A carries an MGM240P**B**32VNA and a project
built for it is compiled with `-DMGM240PB32VNA=1`; on this module that image
boots, runs, and then dies in the radio init with

```
RAIL Assert: 49        (RAIL_ASSERT_INVALID_MODULE_ACTION)
```

leaving the CPU spinning in `_exit` (`sl_syscalls.c:56`), silent on every UART.
Measured 2026-09-17 by halting the running target over SWD and reading the PC
and the task stack. Build for the module id, `MGM240PA32VNA`, and the same
example runs. The board files remain a useful reference for the pin values;
they are where this project's bootloader config headers came from.

## Toolchain

Installed and bench-proven 2026-09-17. The environment block is
`platform/silabs/toolchain.env`; source it before any build. It is an explicit
env block for the same reason the nRF port has one: nothing here is discovered,
every path and version is written down.

Since Simplicity SDK 2025.12 Silicon Labs delivers the SDK as Conan packages
through **SLT-CLI** (the Silicon Labs Tool) and no longer publishes it on
GitHub: the public `SiliconLabs/simplicity_sdk` repository stops at `v2025.6.3`
(last pushed 2026-02-23) and Simplicity Studio's own SDK manager clones from
that same repository. SLT is a standalone public download, no login:
`https://www.silabs.com/documents/public/software/slt-cli-1.2.1-linux-x64.zip`.
The SDK is **not** cloned from git; anyone reproducing this uses the lock file.

| Component | Version | Provenance |
|---|---|---|
| SLT-CLI | 1.2.1-101 | `slt --version` |
| Simplicity SDK | **2025.12.3** | `simplicity-sdk/2025.12.3@silabs#6aa9cc1b19b6623422bce6b44886d329` |
| Silicon Labs Matter | **2.8.1-1.5** (extension 2.8.1) | `matter_extension/2.8.1@silabs#c5f79c12e1928c2887570ef5026eabef` |
| GNU Arm Embedded | 12.2.Rel1, build arm-12.24 (`arm-none-eabi-gcc 12.2.1 20221205`) | `gcc-arm-none-eabi/12.2.rel1@silabs#a2910f647691a3764cd3e48412673f25` |
| slc-cli | 6.0.23 | `slc --version` |
| Java (for slc-cli) | 21.0.6 | ships with slc-cli, must be first on `PATH` |
| ZAP | 2026.06.17 | the extension's stated minimum is v2025.12.02 |
| Simplicity Commander | 1.24.3 (`1v24p3b1989`) | used for `gbl create` and post-build, not for flashing |
| CMake | 3.30.2 | unused by this port's build |
| Ninja | 1.12.1 | unused by this port's build |
| openocd | 0.12.0+dev-02533-gf92f577cc (2026-05-28) | `/usr/local/bin/openocd`, the SWD path |

The recipe is `~/silabs/pkg.slt` and the lock, with the Conan revisions above,
is `~/silabs/pkg.lock`. Reinstall elsewhere with
`slt install -f pkg.lock --check-updates=false --non-interactive`.

The Conan revision is the identity that matters, because that is what was
installed and built against. For reading the SDK sources against a commit, the
GA tree is mirrored at `github.com/SiliconLabsSoftware/sisdk-release`, where
tag `v2025.12.3` is `941f75df141392f802d3834c3ee6537f20000d15` (the same commit
as `v2025.12-build.2712`). That mirror is for reference only; nothing here is
built from it.

**Why extension 2.8.1 and not 2.8.0**, which is what the design spec names:
2.8.1 is the release paired with the newest 2025.12 patch. `slt list
matter_extension -v 2.8.1 --deps` names `simplicity-sdk 2025.12.3`, while
2.8.0 names 2025.12.1. Both are the Matter 1.5 line the spec asks for
(2.8.0-1.5 and 2.8.1-1.5).

**There is no GN in this path.** `scripts/examples/gn_silabs_example.sh` and the
standalone `SiliconLabs/matter` repository are the 2.3.x era; that repository's
newest tag is `v2.3.1-1.3`. Silicon Labs Matter 2.4 and later build through
slc-cli plus make, driven by the extension's own `slc/build.sh`.

### Building a stock example

`slc/build.sh` writes its `out/` tree beside the `.slcp` it is given, which
would put build output inside the Conan package cache where an `slt update` can
drop it. So the extension is copied out of the cache once (see the tail of
`toolchain.env`) and built from the copy at `$MATTER_EXT_ROOT`:

```bash
source platform/silabs/toolchain.env
cd "$MATTER_EXT_ROOT"
./slc/build.sh slc/apps/lighting-app/thread/lighting-app.slcp MGM240PA32VNA
```

Output lands in `out/MGM240PA32VNA/lighting-app/`:

| File | Size (2026-09-17) | Note |
|---|---|---|
| `artifact/lighting-app.s37` | 2 952 636 B | the image openocd programs |
| `build/debug/lighting-app.out` | 83 881 700 B | ELF with debug info, for `addr2line` |
| `artifact/lighting-app.gbl` | 984 268 B | **not** produced by the build; see below |

The `.slcp` build does not emit a `.gbl`. The bootloader takes one, so make it
with Commander:

```bash
commander gbl create lighting-app.gbl --app lighting-app.s37
```

Build figures printed by the post-build step, 2026-09-17, from
`out/MGM240PA32VNA/lighting-app`: flash **1 025 140 / 1 540 096 B (66.56 %)**,
RAM 262 140 B in full (`.bss` 126 584, heap 127 168, `.data` 3 328, stack
4 608). The same example built for BRD2704A came to 1 029 604 B (66.85 %); that
image does not run on this module, see "Do not build for BRD2704A" above.

Application base is **0x08006000**: the bootloader region is the first 24 KiB
of main flash (0x08000000 to 0x08006000).

### The stock example has no usable console here

`-DSILABS_LOG_OUT_UART=1` is in the build and `SL_CATALOG_UARTDRV_EUSART_PRESENT`
is in the component catalogue, but nothing was ever observed from the running
example on any UART: not on PA00/PA01, which is where the module target puts
`sl_uartdrv_eusart_vcom` by default and where the Debug Probe's UART CDC is
wired, and not on PA05/PA06 after the config header was edited to move it
there and the project rebuilt with `--skip_gen`. The example was proven alive
by other means (below). Task 8 wires Hearth's own console; do not take this
silence as evidence that a console cannot work, only that the stock example did
not give one.

### What proved the example alive

1. Over SWD, halting the running target: the PC sits in
   `sli_power_manager_apply_em` (`sl_power_manager_hal_s2.c:631`), the normal
   idle sleep loop, on the process stack. (The BRD2704A build, by contrast, sat
   in `_exit`.)
2. Over BLE, the commissionable advertisement:

   ```
   $ bluetoothctl     # scan le, then info on the address found
   Device D2:07:BE:7B:E6:63
     Name: SL-Light
     UUID: Unknown   (0000fff6-0000-1000-8000-00805f9b34fb)
     ServiceData.0000fff6-...
   ```

   `scan le` matters: a plain `scan on` reports nothing here.
3. Commissioned and controlled, recorded under "Commissioning" below.

## Flashing

Pending (Task 7): `fw/flash.py` over the UART XMODEM bootloader. What Task 5
used by hand, and what Task 7 replaces, is recorded here.

### Installing the bootloader over SWD

Simplicity Commander drives J-Link probes only, and the bench probe is a
Raspberry Pi Debug Probe (CMSIS-DAP), so the bootloader goes in with openocd.
Config file:

```tcl
source [find interface/cmsis-dap.cfg]
transport select swd
reset_config none
source [find target/silabs/xg24.cfg]
```

openocd's image loader does not take the `.s37` directly, so convert first:

```bash
commander convert hearth-bootloader-uart-xmodem-crc.s37 -o hearth-bootloader-uart-xmodem-crc.hex
openocd -f mg24.cfg -c "init" \
        -c "program hearth-bootloader-uart-xmodem-crc.hex verify reset" -c "shutdown"
```

Two openocd notes worth keeping: `exit` in a `-c` script is deprecated, use
`shutdown`; and several `-c` commands joined with semicolons into one string
swallow their output, so give each command its own `-c`.

Never issue an SE device erase or a debug-lock command. `flash erase_sector 0 0
last` is enough when a clean part is wanted, and was used once here.

### Uploading an application over XMODEM

The bootloader menu answers on the AT UART at 115200 after a reset. On this
carrier the CDC's DTR line holds RESETn low and its RTS line pulls the PC00
strap low, and Linux asserts both when the port is opened, so both must be set
explicitly with pyserial after opening:

```python
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False; s.rts = False          # release reset and strap
s.rts = True                          # optional: hold the recovery strap
s.dtr = True; time.sleep(0.15); s.dtr = False   # reset pulse
```

The Gecko Bootloader's parser is **128-byte blocks only**
(`XMODEM_DATA_SIZE 128` in `btl_xmodem.h`), so this is plain XMODEM-CRC, not
XMODEM-1K. Send `1`, wait for the bootloader's `C`, then the blocks. Measured
2026-09-17: 984 268 B as 7 690 blocks in **111 s**, ending

```
Serial upload complete
```

`sx` from lrzsz is the obvious alternative and is the wrong tool here: opening
the port asserts DTR, which holds the module in reset.

### Recovery semantics

| Reset with | Result |
|---|---|
| strap released, valid application | application runs, no menu |
| strap released, no valid application | bootloader menu |
| strap held (PC00 low), valid application | bootloader menu |

All three measured 2026-09-17. The middle row is what let the first upload
happen with no strap at all: the module was blank.

## Bootloader

The project is `platform/silabs/bootloader/`: a `.slcp` derived from the
Simplicity SDK sample `bootloader-uart-xmodem`, plus the two config headers the
bare module target leaves unset. Generated and built with:

```bash
source platform/silabs/toolchain.env
slc generate -d <builddir> --sdk-package-path "$SISDK_ROOT" \
    -p platform/silabs/bootloader/hearth-bootloader-uart-xmodem.slcp \
    --with MGM240PA32VNA --generator-timeout=180 -o makefile
POST_BUILD_EXE=$(which commander) make all -C <builddir> \
    -f hearth-bootloader-uart-xmodem.Makefile -j8
```

A project's own `config/` directory is **not** picked up by slc on its own; the
`config_file:` / `override:` stanzas in the `.slcp` are what bind these headers
to `bootloader_gpio_activation` and `bootloader_uart_driver`. Without them slc
regenerates the SDK defaults, which leave `#warning "GPIO activation port not
configured"` in place.

| Fact | Value |
|---|---|
| Version string on the wire | `Gecko Bootloader v3.02.01` |
| Image | `artifact/hearth-bootloader-uart-xmodem-crc.s37`, 34 514 B |
| Occupies | 0x08000000 to 0x08002CC8, 11 464 B of the 24 KiB region |
| Entry pin | PC00, active low, `SL_BTL_BUTTON_PORT gpioPortC` / `SL_BTL_BUTTON_PIN 0` |
| UART | USART0, PA05 TX, PA06 RX, 115200 8N1, no flow control, `SL_VCOM_ENABLE 0` |
| Signing | none; unsigned `.gbl` accepted, a pre-ship step |

It replaced the module's factory `Gecko Bootloader v2.00.00`, whose GPIO
activation pin was never documented anywhere this project could read. That is
the whole reason the project builds its own.

## Commissioning

Recorded 2026-09-17, stock Silabs `lighting-app` built for MGM240PA32VNA,
flashed as a `.gbl` through the bootloader above, against the bench border
router.

The Thread operational dataset is a credential. `ot-ctl` could not be used: the
control socket `/run/openthread-wpan0.sock` is `root:root` and a non-root user
cannot connect to it. otbr-agent's D-Bus interface serves the same value
without sudo and without touching the daemon:

```bash
DS="$(busctl --system get-property io.openthread.BorderRouter.wpan0 \
        /io/openthread/BorderRouter/wpan0 io.openthread.BorderRouter \
        ActiveDatasetTlvs --json=short \
      | python3 -c 'import json,sys; print("".join("%02x"%b for b in json.load(sys.stdin)["data"]))')"
CT=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
"$CT" pairing ble-thread 0x51 "hex:$DS" 20202021 3840 \
      --storage-directory /tmp/ct-silabs-example 2>&1 | sed -E 's/[0-9a-fA-F]{24,}/[HEX]/g'
```

The dataset stays in a shell variable, is never printed, and every hex run of
24 characters or more in chip-tool's output is redacted. Result:

| | |
|---|---|
| Node id | `0x51` |
| Fabric | compressed id `9F3A67057A34079E` |
| `onoff toggle 0x51 1` | endpoint 1, cluster `0x0000_0006`, command `0x0000_0002`, **Status 0x0 SUCCESS**, exit 0 |
| `basicinformation read vendor-name 0x51 0` | `Silabs` |
| `basicinformation read product-name 0x51 0` | `SL_Sample` |

The toggle and the two attribute reads went over the operational Thread
session, not BLE, so they prove the device joined the mesh.

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
  (`hearth_port_sl.c`) is replaced with the part define the SiSDK project
  sets for the MGM240PA32VNA3. Task 5 measured it: **`MGM240PA32VNA`**, set
  as `-DMGM240PA32VNA=1` when the project is generated with
  `--with MGM240PA32VNA`. Note that a project generated for BRD2704A gets
  `MGM240PB32VNA` instead and RAIL-asserts on this module, so the define is
  not a cosmetic difference; see "Do not build for BRD2704A" above.
- `sl_system_init()` against `sl_main`. The stock 2.8.1 `lighting-app.slcp`
  lists the `sl_main` component, not `sl_system`, and the Matter 2.8.0
  release notes carry an "sl_system to sl_main migration" entry. SiSDK
  2025.12.3 ships both (`platform/service/system/inc/sl_system_init.h` and
  `platform/service/sl_main/inc/sl_main_init.h`, plus a compatibility shim at
  `platform/service/sl_main/sl_system_compatibility/inc/sl_system_init.h`),
  so `hearth_port_sl.c`'s call should still resolve; confirm which one the
  Hearth project actually pulls in before relying on it.
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
