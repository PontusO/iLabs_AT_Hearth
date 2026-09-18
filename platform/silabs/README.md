# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **round 1 (bring-up, downward port, boot contract) is complete on
`dev/silabs-mg24-bringup`, 2026-09-17.** The Hearth skeleton boots on the
MGM240PA32VNA3, prints its boot log on the console and answers the `AT+MT`
surface: `+MTREADY` on the wire, the harness Phase 0 gate passed and Phase 1
run (Task 6), and `fw/flash.py` flashes it from the host over XMODEM, ending at
`+MTREADY` (Task 7). There is no Matter stack in this image: it arrives with
the upward-port round, whose starting checklist is "The 35 failing rows" below
(graph T446).

The third Hearth platform, mimicking the nRF54L15 port: a Thread FTD + BLE
co-processor serving the `AT+MT` contract over one UART. Design:
`iLabs_Hearth_docs/superpowers/specs/2026-09-05-silabs-mg24-port-design.md`.

This README is the platform bible, in the shape of `platform/nrf54l15/README.md`:
the board contract, the toolchain, flashing, and the measured figures are
recorded here as each lands. Every figure below was measured, and says when and
by which command; everything round 1 did not finish is listed with its owner
under "What round 1 leaves open" at the end.

## Board: the iLabs RP2350 carrier with an MGM240PA32VNA3

| Line | Module pin | Signal | Note |
|---|---|---|---|
| AT UART TX | PA05 (pin 12) | EUSART0 TX (app), USART0 TX (bootloader) | the bootloader's TX |
| AT UART RX | PA06 (pin 13) | EUSART0 RX (app), USART0 RX (bootloader) | the bootloader's RX |
| Console TX | PA00 (pin 7) | USART0 TX | Hearth's console, 115200 8N1, TX only; **carries Hearth's boot log since Task 6** (2026-09-17), read on the Debug Probe's UART CDC with DTR asserted. The stock Silabs example never drove it, which is a fact about that example: see "The stock example has no usable console here" |
| Console RX | PA03 (pin 10) | USART0 RX | on the header, deliberately uninitialised by the port: a shipping image has no console input path (design spec board contract item 6, CRA_COMPLIANCE.md) |
| Reset | RESETn (pin 31) | active low | internal pull-up; drive low only, never high |
| SWD | PA01 (pin 8), PA02 (pin 9) | SWCLK, SWDIO | bootloader install, debug. **PA01 is SWCLK: never hand it to a UART** |
| Recovery strap | PC00 (pin 22) | `SL_BTL_BUTTON`, active low | held low through reset enters the bootloader. Task 5 measured all three reset cases, held and released, with and without a valid application ("Recovery semantics"); Task 7's `fw/flash.py` drives it through the CDC's RTS |
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

Both SLT files are in the repository, so the pin outlives any one machine:

- `platform/silabs/slt/pkg.slt`, the recipe, with every version written out
  rather than left as `"~"`.
- `platform/silabs/slt/pkg.lock`, the lock, which additionally carries the
  Conan revisions in the table above and the two dependencies SLT resolved on
  its own (`python 3.10.3` and `java21 21.0.6`). **The lock is the
  authoritative pin.**

Reinstall elsewhere with:

```bash
slt install -f platform/silabs/slt/pkg.lock --check-updates=false --non-interactive
```

`--check-updates=false` matters: the flag defaults to true and would walk the
pins forward.

**That command has not been exercised.** Round 1 installed the toolchain once,
on this machine, and never reinstalled from the lock; the recipe above is the
documented route, read off SLT's own interface, not a reproduced one. The lock
file is what is proven: it is the artefact SLT wrote for the install everything
below was built with. Treat the command as the intended path and expect to
correct it the first time someone actually installs elsewhere.

The Conan revision is the identity that matters, because that is what was
installed and built against. For reading the SDK sources against a commit, the
GA tree is mirrored at `github.com/SiliconLabsSoftware/sisdk-release`:

```
$ git ls-remote --tags https://github.com/SiliconLabsSoftware/sisdk-release.git | grep v2025.12.3
941f75df141392f802d3834c3ee6537f20000d15	refs/tags/v2025.12.3
997a7da21d6111a825259ccb2b9015e72b4e1cc3	refs/tags/v2025.12.3-1
```

`v2025.12.3` is `941f75df141392f802d3834c3ee6537f20000d15`, the same commit as
`v2025.12-build.2712`. Note the separate `v2025.12.3-1` tag on a different
commit; the installed package is 2025.12.3, so `941f75df` is the one to read.
The mirror is for reference only; nothing here is built from it.

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
is in the component catalogue, and nothing has been observed from the running
example on any UART in any of three pin configurations, all rebuilt with
`--skip_gen` after editing `config/sl_uartdrv_eusart_vcom_config.h` and
re-uploaded as a `.gbl`:

| `sl_uartdrv_eusart_vcom` pins | Read on | Result |
|---|---|---|
| PA00 TX, PA01 RX (the module target's default) | Debug Probe UART CDC, DTR cleared | nothing, and the reading is void: see the second bullet below |
| PA05 TX, PA06 RX | the carrier's AT CDC, DTR cleared (correct for the bridge) | nothing |
| PA00 TX, PA03 RX (the board contract's console) | Debug Probe UART CDC, DTR asserted | one or two bytes of framing noise at seven baud rates, no data |

Only the last two rows are readings; the first is kept because the
configuration it names is the trap, not because its result means anything.

Two mistakes were made reaching that table, and both are worth knowing because
they cost bench time and would cost it again:

- **The module target's default RX pin is PA01, which is SWCLK.** The first
  configuration therefore drove a UART receiver onto the pad the Debug Probe
  was actively clocking. That is a real defect in the experiment, and it was the
  obvious candidate explanation for the silence. It is not the explanation: the
  third row above is the same build with the console on the board contract's
  own pins, PA00 TX and PA03 RX, with nothing touching SWCLK, and it is silent
  too. Never let a generated project keep PA01 for a UART.
- **The Debug Probe's UART CDC discards output while the host holds DTR low**
  (`platform/nrf54l15/README.md`, where it cost half a bench day). Every
  Debug-Probe-side reading taken before this was made with DTR cleared and is
  void. With DTR asserted, a scan of 115200, 921600, 230400, 460800, 57600,
  38400 and 9600 baud, eight seconds each after a reset, returns one or two
  non-printable bytes at every rate and nothing else, which is the signature of
  a single line transition rather than of data at the wrong speed.

That last point left one thing genuinely unproven when Task 5 ended: a single
edge is weak evidence that the probe's RX is really on PA00 at all, as opposed
to floating. The path had to be established with traffic this project controls
before silence on it could be trusted.

**Settled 2026-09-17 (Task 6): the path is real and it is on PA00.** The ruling
of that date moved Hearth's own console forward into Task 6, out of the docs
task it had been left to, for exactly this reason, and the first skeleton image
put four lines on the Debug Probe's UART CDC at 115200 with DTR asserted (see
"Measured"). So the probe's RX does reach
PA00, and the stock example's silence was the example's, not the wiring's. The
one thing Task 5 did read there, a single non-printable byte per reset, shows
up in Hearth's capture too: a lone `\x00` at t=0, the line transition when the
module comes out of reset and the TX pad is driven high. It is framing noise,
not data.

The example was proven alive by other means (below). Do not take this silence
as evidence that a console cannot work here, only that the stock example did
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

## Building

The Hearth project is `platform/silabs/hearth.slcp`, an slc project against the
Simplicity SDK alone: there is no Matter in the skeleton image, so the Matter
extension is not in this build at all. `core/sources.cmake` is the source list
of record for `core/`; the `.slcp` writes the same seven paths out in slc's
syntax and says so at the top, and the two must be changed together. That
agreement is enforced since round 2 Task 1 by `test/host/check_slcp_sources.py`,
which `make -C test/host run` calls: it compares the `.slcp`'s `source:` block
with `HEARTH_CORE_SOURCES` and fails on a difference in either direction.
Neither direction is caught by a build. A path added to `sources.cmake` and
forgotten in the `.slcp` shows up only as a link error for the missing symbol,
and a source dropped from `sources.cmake` while the `.slcp` still lists it is
not caught at all: the file is still on disk, so slc compiles it and the build
is green against a list that is no longer the list of record.

The entry point is **`sl_main`**, so the project has no `main()` of its own:
`src/main.cpp` provides the `app_init_early()` and `app_init()` hooks and the
SDK owns `main`. See "Migrating to sl_main" below for what the move changed.

```bash
cd <repo root>
source platform/silabs/toolchain.env
git status --porcelain            # build from a committed tree
slc generate -d ~/silabs/work/hearth-core --sdk-package-path "$SISDK_ROOT" \
    -p platform/silabs/hearth.slcp --with MGM240PA32VNA \
    --generator-timeout=180 -o makefile
POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-core \
    -f hearth.Makefile -j8
cd ~/silabs/work/hearth-core/build/debug
commander gbl create hearth.gbl --app hearth.s37
```

`~/silabs/work/hearth-core` is round 2's build directory. Round 1's
`~/silabs/work/hearth-skeleton` is left where it is: the figures under
"Measured" that name it are that tree's, and they are not re-derivable from a
directory that has been rebuilt over.

`--with MGM240PA32VNA`, the module id, never a board id; see "Do not build for
BRD2704A". Since Task 6 a wrong target is a compile error rather than a bench
mystery: `hearth_port_model()` names `MGM240PA32VNA` alone.

The build directory is outside the repository, for the same reason the
extension is copied out of the Conan cache: slc writes a large generated tree
and nothing good comes of having it inside a checkout.

Like the stock example's, this build emits no `.gbl`; Commander makes one from
the `.s37`. There is no `artifact/` directory and no post-build size report,
because the `.slcp` declares no `post_build:` profile; `arm-none-eabi-size` is
where the figures below come from.

One build note worth keeping:

- **`bootloader_interface` is not optional.** Without that component the
  generated linker file puts `FLASH` at `ORIGIN = 0x8000000`, on top of the
  project's own Gecko Bootloader, and nothing in the build says so: it
  compiles, Commander makes a `.gbl` from it, and the bootloader writes the
  image over itself. With it, `FLASH` is `ORIGIN = 0x8006000, LENGTH =
  0x178000` and `bootloader_app_properties` comes along, which is what
  `commander gbl create --app` wants. This was caught by reading the `size -A`
  output of the first build, not by anything failing.

Round 1's second note said two `sl_system` deprecation warnings were expected.
They are gone with `sl_system` itself; the build is warning-free, and
`grep -c deprecated` over the build log is the check that says so.

### The console this image drives

Hearth's own console is **USART0 TX on PA00, 115200 8N1, TX only**, brought up
by `hearth_console_init()` in `port/hearth_port_sl.c`, declared in
`port/hearth_console.h` and called once from `app_init_early()` in
`src/main.cpp`, which `sl_main` runs after the clock manager and before the
kernel. It was wired in Task 6 (ruling of 2026-09-17) rather than left to the
upward-port round, because the first bench session must not be blind; the
capture is under "Measured".

The form is **bare emlib** (`USART_InitAsync`, `USART_Tx`, and a write to
`GPIO->USARTROUTE[0]`), not the SDK's `sl_iostream_usart` component. iostream
would add an instance whose generated config defaults the RX pin to PA01, which
is SWCLK, and everything it brings beyond raw TX (RX, buffering, the stdio
retarget) is value this port must not have: a shipping image has no console
input path. Sixty lines of emlib have no defaults to get wrong.

Two properties of it worth knowing before touching the log path:

- Log lines take the console's **own** mutex, never the AT link's `s_tx_lock`.
  FreeRTOS mutexes do not recurse, so one shared mutex would deadlock the first
  time a log line was emitted from inside a link write that held it, and a log
  line must not queue behind a long AT response either.
- Anything logged before `hearth_console_init()` returns is **dropped**: the
  peripheral is not configured yet, and `s_console_ready` is the gate that says
  so.

### Migrating to sl_main

**Done, round 2 Task 1, 2026-09-18.** Round 1 ran on `sl_system` because the
skeleton's `main()` was its own (`src/main.c`) and
`sl_system_implementation_kernel` is the component that requires
`custom_main`. That had a deadline and a conflict, and the round that adds the
Matter stack had to move rather than choose (graph F477): `sl_system` is
`quality: deprecated` in SiSDK 2025.12.3 and goes away in sisdk-2026.6;
`sl_system_implementation_kernel` declares `conflicts: sl_main` while the stock
Silicon Labs Matter 2.8.1 `lighting-app.slcp` lists `sl_main`; and
`sl_system_compatibility`, the aliasing shim, declares `conflicts: kernel`,
i.e. baremetal only, so it was no way out either.

What the move changed, and nothing else did:

- `src/main.c` became `src/main.cpp`, and its `main()` became the two hooks
  `app_init_early()` and `app_init()`, both `extern "C"`. The project has no
  `main()`: with a kernel the SDK's `sl_main_init` component compiles its own
  `src/rtos/main.c` and links `main_retarget.c`'s `__wrap_main` over it
  (`toolchain_settings: gcc_linker_option -Wl,--wrap=main`, in
  `sl_main_init.slcc`).
- The `.slcp` lists `- id: sl_main` and no longer lists `sl_system` or
  `sl_main_custom_main`, and gained `toolchain_settings: cxx_standard
  gnu++17`, the option the extension's own `lighting-app.slcp` sets.
- `main()`'s two explicit calls are gone. `sl_system_init()` is the SDK's
  business now (`sl_main_init()` from `__wrap_main`), and
  `sl_system_kernel_start()` likewise (`sl_main_kernel_start()`), so the two
  deprecation warnings are gone with them.

Where the hooks run, read in the SDK at
`$SISDK_ROOT/platform_core/platform/service/sl_main/`:

| Hook | Called from | When |
|---|---|---|
| `app_init_early()` | `src/sl_main_init.c:353` in `sl_main_init()` | after `sl_clock_manager_init()` (`:319`) and the `device_init` steps, **before** `osKernelInitialize()` (`:362`) |
| `app_init()` | `src/rtos/main.c:38` | on the start task, after `sl_main_second_stage_init()` (`sl_platform_init`, `sl_driver_init`, `sl_service_init`, `sl_stack_init`), kernel running |

So the console comes up in `app_init_early()`: its clocks are up there, and it
is the earliest point at which a log line exists. It creates its mutex before
the kernel starts, which is ordinary FreeRTOS (`configAPPLICATION_ALLOCATED_HEAP`
is 0, so heap\_4's `ucHeap` is a static array and needs no kernel), and it is
the same thing `hearth_link_init()` does later from a task.

The boot order the contract depends on is unchanged: platform init, then the
console, then `mt_at_start()` on a task, with nothing on the AT link before
`+MTREADY`. The bench capture that proves it for this image is under "Measured",
"Round 2 baseline".

One `sl_main` behaviour to know before adding to `app_init()`: the start task
runs `while (sl_main_start_task_should_continue()) app_process_action();` after
the hook, and `sl_main_start_task_should_continue()` returns false by default
(`src/sl_main_kernel.c:105`),
so the start task ends there. Hearth's work therefore goes on its own task,
which is what `app_init()` creates.

## Flashing

`fw/flash.py` is the tool (Task 7, 2026-09-17). It does the whole job: enters
the bootloader, uploads the `.gbl`, starts the application and exits 0 only
when `+MTREADY` comes back. What Task 5 and Task 6 did by hand is kept below
as the explanation of what the script does, not as a procedure to follow.

Every serial device here, and every `--port` the flasher takes, **must be
a `/dev/serial/by-id` path**, never `/dev/ttyACM<n>`. ttyACM numbering changes
whenever USB devices are plugged or unplugged; on this bench ttyACM0 is the
Thread RCP, and a stray write to the wrong device kills `otbr-agent`. The
carrier's CDC is
`/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00` and the Debug
Probe's UART CDC is
`/dev/serial/by-id/usb-Raspberry_Pi_Debug_Probe__CMSIS-DAP__E661745883698A36-if01`.

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

To blank the **application only** and leave the bootloader in place, which is
how the blank-slot path is tested without reinstalling anything:

```bash
openocd -f mg24.cfg -c "init" -c "halt" \
        -c "flash erase_address 0x08006000 0x178000" -c "reset run" -c "shutdown"
```

0x08006000 and 0x178000 are the linker's application region exactly (the
`FLASH` `ORIGIN` and `LENGTH` the `bootloader_interface` component produces),
so nothing below 0x08006000 is touched and the module drops into the menu on
its own at the next reset. Measured 2026-09-17: 1 540 096 B erased in 2.9 s.

### Uploading an application: `fw/flash.py`

```bash
export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
python3 platform/silabs/fw/flash.py --port "$MT_PORT" \
        --image ~/silabs/work/hearth-skeleton/build/debug/hearth.gbl
```

```
image: .../hearth.gbl, 47860 bytes, 374 block(s) of 128
bootloader menu: Gecko Bootloader v3.02.01
  100/374 blocks
  200/374 blocks
  300/374 blocks
  374 blocks, 374 frame(s) sent (0 retransmit(s)), 5.3 s
bootloader: Serial upload complete
uploaded 374 block(s), 47860 bytes
+MTREADY seen: the module is running the new image
```

That transcript is a bench run of 2026-09-17, the Task 6 skeleton reflashed
with the tool that replaced the by-hand procedure. Eight successful runs that
day, across both entry paths, gave the same 374 blocks, the same 5.3 s and no
retransmits, and `test/mt_regression.py --bridge cpico --phase 0` passed after
them.

The exit status is the contract: **0 only when `+MTREADY` was read back** from
the new image. Every failure path exits 1 with one line on stderr saying which
step failed and what the port actually returned.

Options beyond `--port` and `--image`: `--ready-timeout` (default 20 s) and
`--no-strap`, which drives neither reset nor strap and waits for the menu the
port's own open produces.

**`--no-strap` reaches a module with a blank application slot, and only that**,
measured 2026-09-17 both ways. Opening this CDC pulses the module's reset even
with DTR and RTS cleared before the open (a running image answers `+MTREADY`
about 200 ms after the open), so a module parked in the menu with a valid
application in flash is started by that reset before the host can write a
byte, whatever it does afterwards; the flag cannot rescue such a session and
does not pretend to. Against a running application it gives up after the
three-second menu wait, measured 2026-09-17 at 3.0 s wall clock, exit 1:

```
flash.py: no bootloader menu within 3 s; got b'+MTREADY\r\n'. --no-strap needs
a module that reaches the menu on its own, which means a blank application
slot; a module with a valid application is started by the open's own reset and
has to be caught with the strap (drop --no-strap).
```

The `+MTREADY` quoted in that message is the module booting its application
from the open's own reset, so the failure carries the evidence for the
paragraph above.

The blank-slot case was measured by erasing the application region over SWD
(`flash erase_address 0x08006000 0x178000`, the linker's application region
exactly, nothing below it) and flashing the skeleton back with
`flash.py --no-strap`: 374 blocks, `+MTREADY`, twice. The wait is what makes
it work. `--no-strap` must wait for the banner exactly as the strap path
does, because after the open's reset the bootloader needs its ~100 ms to come
up; a version of this script that wrote `1` immediately was proven to fail on
that same blank slot, the `1` landing on a USART that was not listening yet
and the run dying at the handshake timeout with the banner arriving after the
byte that was lost.

There is no application-side entry into the bootloader: `AT+MTBOOTLOADER` is
not part of the wire contract, and the strap is the only way in.

The framer is `fw/xmodem.py`, stdlib only, unit-tested against a scripted
receiver in `fw/test_xmodem.py` (`python3 platform/silabs/fw/test_xmodem.py`,
11 tests, no hardware).

#### What the script does, and why each step is there

The bootloader menu answers on the AT UART at 115200 after a reset. On this
carrier the CDC's DTR line holds RESETn low and its RTS line pulls the PC00
strap low, and Linux asserts both when the port is opened, so both are set
explicitly, **before** the open (the order `test/mt_regression.py`'s
`open_at_port()` uses; setting them after the open lets the default reach the
module first):

```python
s = serial.Serial(); s.port = port; s.baudrate = 115200
s.dtr = False; s.rts = False          # release reset and strap
s.open()
s.rts = True;  time.sleep(0.1)        # hold PC00 low
s.dtr = True;  time.sleep(0.1)        # RESETn low
s.dtr = False                         # released into the bootloader
# ... wait for the banner ...
s.rts = False                         # the strap is sampled at boot only, so
                                      # it is released as soon as the menu is
                                      # there, not after the upload
# ... send b"1", wait for the bootloader's "C", send the blocks, then b"2"
#     to run the application
```

The Gecko Bootloader's parser is **128-byte blocks only**
(`XMODEM_DATA_SIZE 128` in `btl_xmodem.h`), so this is plain XMODEM-CRC, not
XMODEM-1K. The wire, measured:

```
banner:    b'\r\nGecko Bootloader v3.02.01\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\nBL > \x00'
after '1': b'\r\nbegin upload\r\n\x00C'
EOT     -> b'\x06'
tail:      b'\r\nSerial upload complete\r\n\x00\r\nGecko Bootloader v3.02.01\r\n...'
```

Two details in that trace are why the script is not a naive loop. The menu
**echoes seventeen bytes before its first `C`**, so a handshake that reads one
byte at a time for a bounded number of tries is exhausted by the echo: the
script drains the preamble itself and pushes the `C` it finds back into the
reader, so the framer's own handshake still sees the byte that started it. And
the bootloader **returns to its menu** after "Serial upload complete", so the
application is started by sending `2`, not by a reset. A reset with the strap
released would start it too (the recovery table below), but `2` needs no line
handling at all and leaves the strap where it is.

Measured 2026-09-17 by hand, the stock Silabs example: 984 268 B as 7 690
blocks in **111 s**, about 14 ms per block. The Hearth skeleton, twenty times
smaller, by hand the same day: **47 860 B as 374 blocks in 5 s**, no block
retries. `flash.py` reproduces that transfer at the same 374 blocks and
measures it at **5.3 s** over eight runs, its own figure and a different
measurement (it times the whole framer call, not just the block loop).

`sx` from lrzsz is the obvious alternative and is the wrong tool here: opening
the port asserts DTR, which holds the module in reset.

The two CDCs on this bench want **opposite** DTR: the carrier's bridge must be
opened with DTR cleared, or the module is held in reset, while the Debug
Probe's UART CDC must be opened with DTR asserted, or it discards everything
the console sends. Getting the second one wrong looks exactly like dead
hardware, and it did once here; see "The stock example has no usable console
here" above. Hearth's boot log was read with DTR asserted and arrived first
time ("Measured").

### Running the harness against this carrier

`test/mt_regression.py` must be given the bridge contract, or it opens the port
pyserial's way (DTR and RTS both asserted) and holds the module in reset and in
recovery at once, which reads as dead hardware:

```bash
export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
```

`--bridge cpico` clears both lines **before** the open and then waits for the
`+MTREADY` that the open's own brief reset produces, so the first command does
not race the boot. The default, `--bridge challenger`, is the C6 contract and
is unchanged.

### Recovery semantics

| Reset with | Result |
|---|---|
| strap released, valid application | application runs, no menu |
| strap released, no valid application | bootloader menu |
| strap held (PC00 low), valid application | bootloader menu |

All three measured 2026-09-17. The middle row is what let the first upload
happen with no strap at all: the module was blank.

A fourth fact belongs beside that table, because it bounds what any host tool
can do: **opening the carrier's CDC resets the module**, even with DTR and RTS
cleared before the open. Measured 2026-09-17 with a running skeleton: the
image answers `+MTREADY` about 200 ms after the open, every time. It is the
same open-time reset the harness's `--bridge cpico` waits out. The
consequence for flashing is that a module parked in the bootloader menu with a
valid application in flash cannot be picked up by a later host session without
the strap: the open takes it out of the menu first.

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

All of this is the skeleton image, 2026-09-17, built from the committed tree at
`233778c` in `~/silabs/work/hearth-skeleton` by the "Building" recipe above,
running on the MGM240PA32VNA3 on the iLabs RP2350 carrier.

The image was built and flashed twice, from `19e7fe8` and then from `233778c`
(a comment-only change), each from a clean `slc generate`. **The figures below
are the second build's**, with one exception that is a genuine comparison: the
Phase 1 result set was captured on both runs and diffed row for row, and the
two are identical (see "Harness Phase 0 and Phase 1"). The two build logs
differ only in the line number of a deprecation warning, which is the comment
that moved. Nothing else below is a two-run figure, and nothing else below
claims to be.

### Image size

`arm-none-eabi-size ~/silabs/work/hearth-skeleton/build/debug/hearth.out`:

| | Bytes |
|---|---|
| `text` | 47 596 |
| `data` | 176 |
| `bss` | 261 536 |

`arm-none-eabi-size -A` on the same file, which is where the interesting split
is:

| Section | Bytes | Where |
|---|---|---|
| `.text` | 46 780 | flash |
| `.vectors` | 368 | flash, at 0x08006000 |
| `.data` | 176 | RAM, loaded from flash |
| `.nvm` | 40 960 | flash, at the TOP of the application region (`__nvm3Base = __main_flash_end__ - SIZEOF(.nvm)`); NVM3's store, not code |
| `.bss` | 42 924 | RAM |
| `.memory_manager_heap` | 214 512 | RAM, the SDK memory manager taking whatever is left |
| `.stack` | 4 096 | RAM, the C stack `main()` runs on |
| `text_application_ram` | 428 | RAM |
| `.bootloader_reset_section` | 4 | RAM, at 0x20000000 |

The three artifact files, `ls -l` in
`~/silabs/work/hearth-skeleton/build/debug` (the `233778c` build; the listing
was read back on 2026-09-17, the day of the build, and the timestamps are the
link's):

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus   47776 Sep 17 16:49 hearth.bin
-rw-rw-r-- 1 pontus pontus   47860 Sep 17 16:49 hearth.gbl
-rwxrwxr-x 1 pontus pontus  143408 Sep 17 16:49 hearth.s37
```

`hearth.bin` and `hearth.s37` come from the link rule of the generated
`hearth.Makefile`, which runs `arm-none-eabi-objcopy` over `hearth.out` three
times immediately after the link (`-O binary`, `-O ihex`, `-O srec`), so the
"Building" recipe's `make all` produces both without being asked. `hearth.gbl`
is the one file the build does not produce: it is the output of the
`commander gbl create hearth.gbl --app hearth.s37` step of that same recipe,
and `fw/flash.py` reads the same 47 860 B back off disk on every upload
(`image: .../hearth.gbl, 47860 bytes, 374 block(s) of 128`). The `.s37` is
three times the size of the image it carries because S-records are ASCII.

Application image on flash: **47 776 B**, which is `hearth.bin`'s size, the
plain binary of the loaded flash sections. Its span is therefore 0x08006000
(the linker's `FLASH` `ORIGIN`) to 0x08011AA0, and it is **3.10 %** of the
1 540 096 B application region, with NVM3's 40 960 B reserved at the top of
that region. The stock Silabs `lighting-app`, for scale, was 1 025 140 B; the
difference is the whole Matter stack, which this image does not carry.

The RAM rows sum to **262 140 B** (42 924 + 214 512 + 4 096 + 176 + 428 + 4),
4 B short of the part's 262 144. The shortfall is alignment padding, not a
section: the linker file puts `RAM` at `ORIGIN = 0x20000004`, one word above
`BOOTLOADER_RESET_REGION` at 0x20000000, and `.stack` opens with `. =
ALIGN(8)`, so it starts at 0x20000008 and 0x20000004 to 0x20000008 is dead.
`.bootloader_reset_section` is already one of the six rows summed above, so it
is not the missing 4 B. Everything not statically claimed ends up in
`.memory_manager_heap`, which is the same shape the stock example showed: it
runs to exactly 0x20040000, the top of RAM.

### Free FreeRTOS heap at `+MTREADY`

`configTOTAL_HEAP_SIZE` is **24 576** (raised from the SDK default of 8 192,
which does not hold the boot task and the 6 KiB AT parser task at the same
time). `xPortGetFreeHeapSize()` logged immediately after `mt_at_start()`
returns: **13 768 B free**. That figure still counts the boot task's own 4 KiB
stack and TCB, which are released a few instructions later by
`vTaskDelete(NULL)`, so the steady-state figure is about 4 KiB higher. This is
the FreeRTOS heap only; `.memory_manager_heap` is a separate 214 512 B pool
that nothing in the skeleton allocates from.

### The boot log, and `+MTREADY` after it

Captured 2026-09-17 by resetting the module (DTR pulse on the carrier's CDC)
and reading both ports at once with timestamps. The console is the Debug
Probe's UART CDC at 115200 **with DTR asserted**; the AT link is the carrier's
CDC with DTR and RTS cleared.

```
  0.000  CONSOLE  b'\x00'
  0.091  CONSOLE  b'I boot: Hearth skeleton on USART'
  0.101  CONSOLE  b'0 TX PA00, 115200 8N1\r\nI boot: boot task up, model MGM240P Hearth\r\nI at_parser: parser started\r'
  0.101  AT       b'+'
  0.111  CONSOLE  b'\nI boot: +MTREADY sent, free heap 13768 B\r\n'
  0.111  AT       b'MTREADY\r\n'
```

Console, in full:

```
I boot: Hearth skeleton on USART0 TX PA00, 115200 8N1
I boot: boot task up, model MGM240P Hearth
I at_parser: parser started
I boot: +MTREADY sent, free heap 13768 B
```

AT link, in full: `b'+MTREADY\r\n'`, 10 bytes, **nothing before it and nothing
else**. The boot contract is what this capture is evidence for: the first three
console lines are on the wire before the first byte of `+MTREADY`, and no URC
precedes the marker.

The leading `\x00` on the console at t=0 is the line transition as the module
leaves reset and the TX pad is driven high, not data.

### The AT identity surface

```
boot: b'+MTREADY\r\n'
AT           -> OK
AT+CGMM      -> MGM240P Hearth|OK
AT+CGMR      -> 1.2.0|OK
AT+MTVER?    -> +MTVER:1.2.0|OK
AT+MTNET?    -> +MTNET:THREAD,0,0,0|OK
AT+MTSTATE?  -> +MTSTATE:0,0|OK
```

### Harness Phase 0 and Phase 1

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
===== RESULT: 0 passed, 0 failed =====

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico \
      --transport THREAD --phase 1 --baseline platform/silabs/skeleton-phase1.json
===== RESULT: 261 passed, 35 failed =====
baseline written: platform/silabs/skeleton-phase1.json
```

Run twice, sixteen minutes apart, across a reflash of a rebuilt image: the
same 261 and the same 35, **row for row**. That is a diff, not an impression.
Both runs were redirected to files, run 1 from the `19e7fe8` tree and run 2
from the `233778c` tree, and this is the comparison, 2026-09-17:

```
$ grep '^===== RESULT' phase1.log
===== RESULT: 261 passed, 35 failed =====
$ grep '^===== RESULT' phase1b.log
===== RESULT: 261 passed, 35 failed =====

$ diff <(grep 'PASS\]' phase1.log) <(grep 'PASS\]' phase1b.log); echo $?
0
$ diff <(grep 'FAIL\]' phase1.log) <(grep 'FAIL\]' phase1b.log); echo $?
0
```

Both diffs are empty, so it is not only the counts that match: the same 261
rows passed and the same 35 rows failed, by name. The two logs are the same
size to the byte (22 984 B each) and were written at 16:34 and 16:51.

296 rows, the same count the nRF54L15 skeleton ran. The record is
`platform/silabs/skeleton-phase1.json`, kept **here and not in
`test/baselines/`**, which holds shipping baselines only: this image has no
data model and must never be mistaken for a qualified one.

A figure that exists only in a terminal scrollback is not provenance. These two
runs could be compared after the fact because they were redirected to files; the
size and boot figures of the first build were not, which is why the preamble
above says the figures are the second build's. Any later round that means to
claim reproduction should redirect every measurement it intends to compare.

### The 35 failing rows: the upward port's starting checklist

Every one of them needs a Matter data model, and the skeleton answers them from
`port/mt_matter_stub.c` and `port/mt_devtypes_stub.c`. The shape of the stub
answers, sampled by hand:

```
AT+MTCODES?        -> ERROR
AT+MTATTR=1,6,0    -> +MTERR:2|ERROR
AT+MTEP?           -> OK            (an empty composition)
AT+MTTHREAD?       -> +MTERR:8|ERROR
AT+MTROWGET=1,1    -> +MTERR:2|ERROR
```

`+MTERR:2` is endpoint-not-found, which is honest: there are no endpoints.
Several rows expect `+MTERR:3` or `+MTERR:4` (cluster or attribute not found on
an endpoint that does exist), and those are the rows that turn green when the
composition rebuild is real. The list, verbatim from the run:

- `[AT+] MTCODES? format`
- `[AT+] MTCODES? stable across reads`
- `[AT+] MTATTR read 1,6,0`
- `[AT+] MTATTR hex equals decimal`
- `[AT+] MTATTR root VendorID read`
- `[AT-] MTATTR=1,6,0,1,2 -> +MTERR:1`
- `[AT-] MTATTR=1,6,0,z -> +MTERR:1`
- `[AT-] MTATTR=1,0xFFFF,0 -> +MTERR:3`
- `[AT-] MTATTR=1,6,0xFFFF -> +MTERR:4`
- `[AT-] MTATTR NodeLabel -> +MTERR:5`
- `[AT-] MTEP=0x0071,0,0 under a light -> +MTERR:1 (cabinet parent must be a fridge or an oven)`
- `[AT-] MTEP=0x0077 unparented -> +MTERR:1 (cook surface REQUIRES a cooktop parent)`
- `[AT-] MTEP=0x0077,0,0 under a light -> +MTERR:1 (cook surface parent must be a cooktop)`
- `[AT-] MTALARM=1,0,0 -> +MTERR:3 (migrated: field 0 passes the union gate; the single-light rig's ep 1 carries neither alarm cluster)`
- `[AT-] MTATTR=1,6,0,-1 -> +MTERR:1 (minus on an unsigned attribute: pre-round-A this wrapped through strtoul and wrote true)`
- `[AT-] MTATTR=1,6,0,92233720368547758080 -> +MTERR:1 (literal overflows 64 bits: rejected at parse, ERANGE)`
- `[AT-] MTATTR=1,6,0xFFFC,4294967296 -> +MTERR:1 (2^32 into the u32 FeatureMap: the width gate, not silent truncation to 0)`
- `[AT-] MTMEAS staged variant-1 water heater: gate rows (+MTERR:3, gate outranks range), unknown field, then restore`
- `[AT-] MTMEAS=1,152,3,-5000000000 -> +MTERR:3 (AbsMinPower IS signed, so the minus parses and the light's missing cluster answers: the signedness table's positive control)`
- `[AT-] MTDEMCAP/MTMEAS staged variant-1 solar, battery and DEM: the cluster-missing and attribute-missing rows with their OK controls, then restore`
- `[AT-] MTTHREAD? shape by image (+MTERR:8 on WiFi, decoded line on Thread)`
- `[AT-] MTROWAPPLY=1,1,1 -> +MTERR:4 (stage matches, reaches the bridge: ep 1 carries no EnergyEvse cluster), then AT+MTROWCLEAR -> OK`
- `[AT-] MTROWGET=1,1 -> +MTERR:4 (ep 1 carries no EnergyEvse cluster, unqualified bulk form)`
- `[AT-] MTROWGET=1,1,0 -> +MTERR:4 (same lookup, single-row form)`
- `[AT-] MTMETERID comma survives inside a quoted pod -> +MTERR:4 (ep 1 wrong cluster: reaching the bridge at all proves the comma did NOT split the token)`
- `[AT-] MTMETERID 64-byte pod -> +MTERR:4 (ep 1 wrong cluster: reaching the bridge proves the length was accepted)`
- `[AT-] MTMETERID pwr only present -> +MTERR:4 (ep 1 wrong cluster: reaching the bridge proves choice-b accepted a single value)`
- `[AT-] MTMETERID apparent only present -> +MTERR:4 (same, the other half of choice-b)`
- `[AT-] MTMETERID=1,0,"A","B","C",100,200, -> +MTERR:4 (ep 1 wrong cluster: the trailing comma makes an empty <src> legal, null)`
- `[AT-] MTMEAS=1,153,4,-5000000000 -> +MTERR:3 (CircuitCapacity IS signed, so the minus AND the 64-bit width both parse and the call reaches the bridge, where ep 1 carries no EnergyEvse cluster: the +MTERR:3 is the positive evidence)`
- `[AT-] MTMEAS=1,153,15,-1 -> +MTERR:3 (BatteryCapacity is signed too, the same differential)`
- `[AT-] MTMEAS=1,153,18,5000000000 -> +MTERR:3 (SessionEnergyCharged past 32 bits parses, the 64-bit value pipeline: same differential)`
- `[AT-] MTMEAS=1,153,0,1 -> +MTERR:3 (a perfectly well formed push at a light endpoint: cluster-not-present, not endpoint-not-present)`
- `[AT-] MTROWAPPLY count-0, both directions, on a real EVSE endpoint (case a: nothing staged; case b: two rows staged, must be abandoned not committed); SOC-variant rule negative arm; meter identity push + AT+MTATTR readback (the dead-shell fix)`
- `[AT-] Utility meter pool exhaustion (MT_METER_MAX=2): a third meter aborts the rebuild and AT+MTEP? shows exactly the successful prefix, never the declared count and never empty`

Nothing in this list is a defect in the port. The list belongs to the
**upward-port round** (graph T446), which starts from it and from the baseline
it was recorded in, `platform/silabs/skeleton-phase1.json`: each batch of that
round retires stubs and turns rows green, and when it lands, this section's
replacement is the list of rows that still fail.

### What is not measured here

`hearth_log_write` now writes to the console, so the RX ring overflow warning
is observable at last: `W link: rx ring overflow, dropped N byte(s)`. None
appeared in any run above, but none of those runs pushed the link hard enough
for that to be evidence. The ring is 1 024 bytes against a 512-byte
`MT_AT_LINE_MAX`.

## First-compile checklist (Task 6): what each item turned out to be

`hearth_port_sl.c` (Task 4) was written and host-tested for its pure-C pieces
without a Simplicity SDK toolchain, so a list of SDK identifiers and behaviours
was left unverified. Task 6 was the first real compile, 2026-09-17, against
SiSDK 2025.12.3. **Two items were wrong and are fixed; the rest held.** No item
on this list is open: it is the record of the verdicts, kept because the next
person to touch this file will ask the same questions.

| Item | Verdict |
|---|---|
| `GPIO->EUSARTROUTE[0]` field names and the shift macros | **Correct.** `efr32mg24_gpio.h` declares `GPIO_EUSARTROUTE_TypeDef` with `ROUTEEN`, `RXROUTE` and `TXROUTE`, `GPIO->EUSARTROUTE[2]`, and `_GPIO_EUSART_TXROUTE_PORT_SHIFT` 0 / `_GPIO_EUSART_RXROUTE_PIN_SHIFT` 16 exist as used. `EUSART0_RX_IRQn` is IRQ 11. |
| `EUSART_UART_INIT_DEFAULT_HF`'s RX FIFO watermark | **Correct, and it is one frame.** The macro passes `advancedSettings = NULL`, `EUSART_UartInitHf()` writes `CFG1 = _EUSART_CFG1_RESETVALUE`, and `_EUSART_CFG1_RXFIW_DEFAULT` is `RXFIW_ONEFRAME` (0). So `STATUS.RXFL` and `IF.RXFL` do fire per received byte, and the ISR's drain loop is right. The nRF port's lost `'='` cannot recur through a coarse watermark here. |
| The default clock EUSART0 is fed from, and 115200 without drift | **Fine, and it is the 39 MHz HFXO.** `CMU_EUSART0CLKCTRL.CLKSEL` resets to `EM01GRPCCLK`; the project's clock manager takes `SL_CLOCK_MANAGER_DEFAULT_HF_CLOCK_SOURCE` = HFXO, and the MGM240PA32VNA config override sets `SL_CLOCK_MANAGER_HFXO_FREQ` 39000000, the module's own crystal. `EUSART_UartInitHf()` derives CLKDIV from `CMU_ClockFreqGet()`, so nothing is hardcoded: 39 MHz, OVS16, CLKDIV 5160 gives 115 215 baud, 0.013 % off. Bench-confirmed by the whole Phase 1 run. |
| `configUSE_MUTEXES` | **Enabled.** `1` in the SDK's `config/series2/FreeRTOSConfig.h`. |
| `CORE_atomicState_t` | **WRONG, fixed.** No such type. emlib has one type for both section kinds, `CORE_irqState_t` (`typedef uint32_t`, `sl_core.h`), and `CORE_EnterAtomic()` / `CORE_ExitAtomic()` take and return it. The function-pair form itself was right. |
| The Hearth NVM3 key range `0x0A000..0x0AFFF` | **No collision.** CHIP's `SilabsConfig` uses `kMatterNvm3KeyDomain` 0x087000, range 0x087200 to 0x087FFF; the OpenThread EFR32 settings backend uses `NVM3KEY_DOMAIN_OPENTHREAD` 0x20000 upward. Hearth's range sits inside the user domain (0x000000 to 0x00FFFF) that neither touches. |
| The part define for `hearth_port_model()` | **Was a family catch-all, fixed.** The generated makefile carries `-DMGM240PA32VNA=1`, and `hearth_port_model()` now names that alone. A project generated for BRD2704A gets `MGM240PB32VNA` and now fails to compile, instead of building an image that RAIL-asserts on this module. |
| `sl_system_init()` against `sl_main` | **Resolves, via `sl_system`, which is deprecated.** The project lists `sl_system`, whose kernel implementation is the component that requires `custom_main`, which is what lets `main()` be this project's own. It compiles with two `-Wdeprecated-declarations` warnings, left visible on purpose. The migration and its deadline are in "Migrating to sl_main" above; round 2 Task 1 did it, because `sl_system_implementation_kernel` declares `conflicts: sl_main` and the stock Matter 2.8.1 app uses `sl_main`. |
| The RX IRQ priority against `configMAX_SYSCALL_INTERRUPT_PRIORITY` | **Correct, and exactly at the boundary.** `CORE_ATOMIC_BASE_PRIORITY_LEVEL` is 3, `__NVIC_PRIO_BITS` is 4, so `NVIC_SetPriority(EUSART0_RX_IRQn, 3)` writes 3 << 4 = 48 into the IPR byte. `configMAX_SYSCALL_INTERRUPT_PRIORITY` is 48, and FreeRTOS's check is `>=`, so `xSemaphoreGiveFromISR()` from that handler is legal. `CORE_EnterAtomic()` sets `BASEPRI` to the same 48, which masks priority values at or above it, so the atomic sections really do exclude this ISR: both halves of the Task 4 reasoning hold, and they hold because the two numbers are equal, not by a margin. Anything that changes either one breaks both at once. |
| `configTICK_RATE_HZ` | **1000, not 1024.** So `portTICK_PERIOD_MS` is 1 and does not truncate to 0, and `pdMS_TO_TICKS()` cannot overflow a 32-bit `TickType_t` at the header's one-hour ceiling. The tick-rate-independent `hearth_now_ms()` and the 60-second wait slicing are therefore belt and braces on this project as configured, and they stay: the value is one line in a generated config header, and the failure they prevent is silent. |
| The port's second `nvm3_initDefault()` | **A documented no-op.** `sl_platform_init()` (generated `autogen/sl_event_handler.c`) calls `nvm3_initDefault()`, so the lazy `nvm3_ensure_init()` is a second `nvm3_open()` with the same handle and init data, which `nvm3_generic.h` says "will be regarded as a no operation and the function will return the same status as the previous call". Kept, because it costs nothing and keeps the KV store usable if a future project drops the component. Note the return type is `sl_status_t` now, with `ECODE_NVM3_OK` defined as `SL_STATUS_OK`; the comparison is still correct. |
| `EUSART_BaudrateSet()` with the EUSART left enabled | **Required, not merely tolerated.** In asynchronous mode `em_eusart.c` asserts `EFM_ASSERT(eusart->EN == EUSART_EN_EN)` before touching CLKDIV: "The peripheral must be enabled to configure the baud rate." Disabling around the call, which was the suggested remedy, would have introduced the bug. |

Two more things the first build settled that were not on the list:

- **`bootloader_interface` has to be in the project**, or the image links over
  the bootloader silently. See "Building".
- **`vTaskStartScheduler()` is the wrong way to start the kernel here.** The
  generated `sl_kernel_start()` calls `osKernelStart()`, the CMSIS-RTOS2 entry
  that pairs with the `osKernelInitialize()` `sl_platform_init()` already ran.
  `main()` calls `sl_system_kernel_start()`.

## What round 1 leaves open

Round 1 is bring-up, the downward port and the boot contract. Everything it did
not finish is here, and nothing here is unowned.

Round 2 Task 1 (2026-09-18) closed five of these rows and they are gone from
the table: the `sl_system` to `sl_main` migration, `hearth_link_write()`'s
missing peripheral-ready guard, the unchecked `xTaskCreate()` return, the
ad-hoc `hearth_console_init()` declaration, and the unenforced agreement
between `hearth.slcp` and `core/sources.cmake`.

| Open item | Owner |
|---|---|
| The Matter stack: `chip::Server` with the minimal static ZAP, the disabled catalogue endpoint 240, the composition rebuild, the arenas and the device-type catalogue in audited batches, each batch retiring stubs from `port/mt_matter_stub.c` and `port/mt_devtypes_stub.c` | the **upward-port round** (graph T446). Its starting checklist is "The 35 failing rows" above, and the run they came from is the baseline `platform/silabs/skeleton-phase1.json` |
| Signing (ECDSA-P256 dev key under `keys/`, as the nRF port does), secure boot, and the SE debug lock after the one-time SWD install | **pre-ship** (design spec section 7, stage 2). The bootloader itself is built and installed already; it accepts an unsigned `.gbl` today |
| Thread-arm baselines under `test/baselines/`, ARCHITECTURE 8.21 and its decision-log rows, and the host library's `fw/README` variant table | the **qualification round** (design spec section 8, steps 6 and 7). The skeleton's Phase 1 record deliberately stays out of `test/baselines/`, which holds shipping baselines only |
| The RX ring overflow warning is observable but has never fired: no run so far pushed the link hard enough for its absence to be evidence | the **upward-port round**'s first sustained traffic; see "What is not measured here" |
| The console is TX only by design, so the port has no console input path and no shell | settled, not open: it is board contract item 6 and a CRA posture (`CRA_COMPLIANCE.md` in the docs repository). Listed here so nobody reopens it as an omission |
