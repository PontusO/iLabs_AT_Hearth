# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **round 1 (bring-up, downward port, boot contract) is complete on
`dev/silabs-mg24-bringup`, 2026-09-17.** The Hearth skeleton boots on the
MGM240PA32VNA3, prints its boot log on the console and answers the `AT+MT`
surface: `+MTREADY` on the wire, the harness Phase 0 gate passed and Phase 1
run (Task 6), and `fw/flash.py` flashes it from the host over XMODEM, ending at
`+MTREADY` (Task 7). There is no Matter stack in this image: it arrives with
the upward-port round, whose starting checklist is "The 35 failing rows" below
(graph T446).

**Round 2 (the Matter core) has started on the same branch.** Its first task,
2026-09-18, moved the skeleton onto the SDK's `sl_main` entry point and closed
four round 1 deferrals; the image it produced is measured under "Measured",
"Round 2 baseline".

**Task 2, 2026-09-18, put the Matter stack in the image.** The extension is
prepared and patched by `fw/sdk-prepare.sh` behind a revision gate, Hearth's
own data model is generated and committed ("Data model" below), and
`hearth.slcp` carries the Matter components: 728,200 B of `text` against the
skeleton's 48,336, and it boots to `+MTREADY` unchanged, 296 Phase 1 rows
identical to the baseline row for row ("Measured", "Round 2 task 2"). The stack
was linked, not started.

**Task 3, 2026-09-18, started it.** `port/hearth_matter_init.cpp` is Hearth's
own bring-up, and the boot task runs it before `mt_at_start()`: the server is
initialised over the committed data model, the CHIP event loop runs, the
catalogue endpoint is disabled, and the device advertises over BLE as `Hearth`
with the Matter commissionable service UUID. `+MTREADY` still follows the boot
log with nothing before it on the AT port, and Phase 1 is still 261/35, row for
row ("Measured", "Round 2 task 3"; "Boot flow" for what runs in what order).
The upward port is still stubbed, so the same 35 rows still fail; retiring them
is the upward-port round's.

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
Simplicity SDK **and the Silicon Labs Matter extension** since round 2 task 2.
`core/sources.cmake` is the source list
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
platform/silabs/fw/sdk-prepare.sh   # once per SDK install; see "SDK patches"
git status --porcelain              # build from a committed tree
slc generate -d ~/silabs/work/hearth-matter --sdk-package-path "$SISDK_ROOT" \
    --sdk-package-path "$MATTER_EXT_ROOT" \
    -p platform/silabs/hearth.slcp --with MGM240PA32VNA \
    --generator-timeout=180 -o makefile
POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter \
    -f hearth.Makefile -j8
cd ~/silabs/work/hearth-matter/build/debug
commander gbl create hearth.gbl --app hearth.s37
```

**Step one is `fw/sdk-prepare.sh`, and on a machine that has never run it the
`source` above prints two lines and returns 1**: that is the patch gate at the
end of `toolchain.env`, which refuses an extension copy that is unprepared or
prepared at the wrong patch revision. Every variable is exported before the
gate runs, so the shell is still usable and the prepare runs straight
afterwards; source the file again and it returns 0. Details in
`sdk-patches/README.md`.

**Two `--sdk-package-path` arguments**, the SDK and the extension. That is the
form the extension's own `slc/build.sh` uses (line 353, which also passes a
third for the Wi-Fi SDK that this Thread port does not need); one path alone
cannot resolve `hearth.slcp`'s `sdk_extension:` stanza. `slc signature trust`
was never needed: the extension's components generate without it.

`~/silabs/work/hearth-matter-t3` is round 2 task 3's build directory, and the
one the current image came from. Task 2's `~/silabs/work/hearth-matter`, round
1's `~/silabs/work/hearth-skeleton` and task 1's `~/silabs/work/hearth-core` are
left where they are: the figures under "Measured" that name them are those
trees', and they are not re-derivable from a directory that has been rebuilt
over.

One thing the prepare takes with it, worth knowing before running it: it
replaces `$MATTER_EXT_ROOT` **whole**, so an `out/` tree from building a stock
example inside it (the recipe under "Building a stock example") goes too. The
round 1 stock-light figures below stand on their own record; rebuild the example
if it is wanted again.

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
`grep -c deprecated` over the build log is the check that says so. **It is still
warning-free with the whole Matter stack in it**, `grep -ci warning` 0 as well,
and keeping it that way cost one component: see "The components, and the three
that could not stay".

### The components, and the three that could not stay

Task 2's component set is the Silicon Labs Matter samples' minus the shell, the
OTA support and requestor, the LCD, the buttons and LEDs, and the lighting
application. `hearth.slcp` carries the reasoning next to each line; three
entries are worth repeating here, because each was a build failure and not a
preference.

- **`matter_platform_mg` could not stay.** It is what every sample lists, and it
  compiles the sample APPLICATION framework alongside the platform layer:
  `BaseApplication.cpp` and `MatterConfig.cpp`, which include `"AppEvent.h"`,
  `"AppConfig.h"` and `<AppTask.h>` and call
  `AppTask::GetAppTask().StartAppTask()`. With it in the project the build stops
  at `BaseApplication.cpp:25:10: fatal error: AppEvent.h: No such file or
  directory`. The headers are the sample application's; supplying Hearth
  versions of them would mean carrying the sample's app-event, button, LED and
  LCD framework to satisfy a bootstrap this product does not use. **The CHIP
  platform layer it wraps is the `efr32` component**, which `hearth.slcp` lists
  instead (`PlatformManagerImpl`, `BLEManagerImpl`, `ThreadStackManagerImpl`,
  `ConfigurationManagerImpl`, `KeyValueStoreManagerImpl`, `CHIPMem-Platform`,
  `Logging`, the PSA keystore), together with the platform components
  `matter_platform_mg` required and nothing else pulls in: `sleeptimer`,
  `dmadrv`, `mpu`, `udelay`, `component_catalog`, `rail_lib_multiprotocol`,
  `hal_wdog`, `gpiointerrupt`, `emlib`. Hearth's own Matter bootstrap, for which
  `MatterConfig.cpp` is the reference to read, belongs in the port and arrives
  with task 3.
- **`matter_temperature_measurement` does not exist.** `slc` stops with
  "Referenced project component matter_temperature_measurement not in
  framework", and nothing is missing: Temperature Measurement has no cluster
  server in connectedhomeip (there is no `temperature-measurement-server`
  directory) and no row in the extension's own
  `cluster-to-component-dependencies.json`. It is served entirely out of ember
  attribute storage, which for this port means the external-attribute callbacks
  the upward port provides.
- **`rail_util_pti` went with `matter_platform_mg`**, which was the only thing
  requiring it. On the bare module target the generated config sets
  `SL_RAIL_UTIL_PTI_MODE` to `SL_RAIL_PTI_MODE_DISABLED` and leaves the pin
  block unset, so it contributed one disabled peripheral and the build's only
  warning (`#warning "RAIL PTI peripheral not configured"`). This carrier does
  not route the Packet Trace pins; the sample dev kits do, which is why the
  samples carry it.

Two components are here for the opposite reason, demanded rather than chosen:
`matter_configuration_over_swo`, because `matter` requires the
`configuration_over_swo` api and the Simplicity SDK's own provider drags in
`iostream_rtt` and SEGGER RTT; and `matter_segger_rtt`, because
`matter_provision_default` requires `iostream_rtt` which requires `segger_rtt`,
and `matter_provision_default` is not optional (`matter` requires the
`matter_provision` api, and the only other provider requires `iostream_rtt`
too). So SEGGER RTT is in the image whether or not anything uses it.

**Where CHIP's own log output goes: the console, since round 2 task 3.**
`src/platform/silabs/Logging.cpp` writes to `SEGGER_RTT_WriteNoLock` unless
`SILABS_LOG_OUT_UART` is 1, and setting that needs `matter_uart`, the SDK's UART
driver, on a peripheral Hearth drives itself with bare emlib. Task 2 therefore
recorded that CHIP logs reach neither of this board's UARTs. Task 3 took the
third way, `chip::Logging::SetLogRedirectCallback()`, which needs no SDK driver
and no strong symbol; see "CHIP's log output is on the console now" under "Boot
flow". Two sinks still go to RTT and always will: `silabsLog()` and
`otPlatLog()`, which call `PrintLog` directly. The AT link is untouched either
way, which is the property that matters.

### The heap changed shape: there is no FreeRTOS heap any more

`matter_platform_mg`'s replacement chain requires **`freertos_heap_3`**, and
heap_3 and heap_4 are exclusive (both provide the `freertos_heap` api), so slc
refuses a project carrying `freertos_heap_4` and the Matter platform at once:

```
Exclusivity Issue with freertos from silabs.simplicity_sdk from freertos_heap
within rule Requires freertos_heap: ... freertos_heap_4 ... freertos_heap_3 ...
```

heap_3 forwards `pvPortMalloc()` to the C library's `malloc()`, which the SDK's
linker wraps into `sl_memory_manager`'s `.memory_manager_heap`. Two consequences
that a memory measurement here has to respect from now on:

- **`configTOTAL_HEAP_SIZE` is gone from `hearth.slcp`**, not raised. Under
  heap_3 there is no `ucHeap` array for it to size and nothing reads it. Round
  1 and round 2 task 1 measured a 24,576 B FreeRTOS pool; that pool does not
  exist in this image.
- **`xPortGetFreeHeapSize()` is gone from `src/main.cpp`.** `heap_3.c` does not
  define it, which is a link error rather than a wrong number.
  `sl_memory_get_free_heap_size()` replaced it, and it measures the one pool
  that now holds kernel objects, CHIP's allocations and everything else.

The design spec's section 6 asks for RAM in three parts, one of them "the
FreeRTOS heap high-water mark ... 24,576 B configured". That part does not apply
to this platform as built; the two pools are one.

### Data model

`data_model/hearth.zap` is the data model, and `fw/zap-regen.sh` is the only
thing in this repository that writes `data_model/zap-generated/`. The generated
files are committed and **never hand-edited**; a data-model change is one `.zap`
edit and one script run, in one commit.

```bash
source platform/silabs/toolchain.env
platform/silabs/fw/zap-regen.sh          # write data_model/zap-generated
platform/silabs/fw/zap-regen.sh --check  # regenerate to a temp dir and diff
```

The file started as the nRF54L15 arm's `src/default_zap/hearth.zap`, which is
why the two arms' models are comparable. What task 2 changed:

- **Endpoint 240, the disabled catalogue endpoint**, keeps Identify, Groups,
  On/Off, Descriptor and Temperature Measurement enabled and every other
  catalogue cluster `"enabled": 0`. The entries are disabled, not deleted: a
  later batch re-enables the clusters it needs with one flag each.
- **Endpoint 0** keeps the root-node set with **OTA Software Update Requestor
  and Provider disabled**: the update story is host-driven serial flashing
  (`FIRMWARE_UPDATE_SPEC.md`), deliberately not Matter OTA. **Its
  `MA-otarequestor` DEVICE TYPE went with them**, so endpoint 0 declares Root
  Node 0x0016 alone and `FIXED_DEVICE_TYPES` reads
  `{{0x00000016,4},{0x00000101,3}}`. Disabling the clusters without removing the
  device type left the root node advertising device type 0x0012 whose mandatory
  cluster 0x002A is absent, which a certification tool reads as a broken
  declaration and a controller can see the moment the stack runs. Endpoint 0 is
  never disabled, so this one was not cosmetic.
- **Endpoint 240's device type 0x0101 (dimmable light) is left as it is**, with
  Level Control disabled under it. It is the same shape of inconsistency and it
  is deliberately not the same problem: this endpoint is the CATALOGUE, the port
  disables it at runtime before any fabric can read it (which is what "disabled
  catalogue endpoint" means and is task 3's `emberAfEndpointEnableDisable`
  call), and its device type list is never served. Removing it would diverge
  this arm's model from the nRF arm's, which carries the same entry, for no wire
  effect. A known artefact, recorded here so nobody has to rediscover it; the
  round that gives endpoint 240 a device type it means is the one that should
  change it, for both arms.
- The two `"package"` paths point at the extension's `zcl.json` and
  `app-templates.json` as **absolute paths on this machine**, exactly as the nRF
  file points at its NCS workspace. They are overridden at every generation, by
  `-z`/`-g` from `zap-regen.sh` and by the SDK-provided properties slc reads out
  of `matter.slsdk`, so nothing depends on them resolving; they are a record of
  which tree the file was last edited against.

**Round 2 task 3 disabled Diagnostic Logs (0x0032) and Wi-Fi Network
Diagnostics (0x0036) on endpoint 0.** Both were enabled with no server component
in `hearth.slcp`, which is not a link error (the generated `callback-stub.cpp`
defines every cluster init callback weakly) but is metadata with nothing behind
it, and the second is Wi-Fi diagnostics on a Thread-only product besides. Task 2
kept them because the data model is the nRF arm's and the two Thread ports
should answer the same; task 3 started the stack, at which point a controller
could read them, and a cluster that answers nothing is worse than an absent one.
They are disabled rather than deleted, so re-enabling either is one flag and one
`fw/zap-regen.sh` run. **The nRF arm still carries both**, and reconciling the
two arms' root-node surface is still the qualification round's.

Generated figures, read out of `data_model/zap-generated/endpoint_config.h`
(2026-09-18, zap 2026.6.18):

| | |
|---|---|
| `FIXED_ENDPOINT_COUNT` | `(2)` |
| `FIXED_ENDPOINT_ARRAY` | `{ 0x0000, 0x00F0 }` |
| `FIXED_DEVICE_TYPES` | `{{0x00000016,4},{0x00000101,3}}` |
| `GENERATED_CLUSTER_COUNT` | 16 (18 before task 3 disabled the two above) |
| `ATTRIBUTE_LARGEST` | `(66)` |
| `ATTRIBUTE_MAX_SIZE` | `(40)` (46 before task 3) |
| `ATTRIBUTE_SINGLETONS_SIZE` | `(0)` |

`ATTRIBUTE_LARGEST` is the size of the generated ember IO buffer, and it is the
number the Descriptor declaration has to stay under: the upstream bridge
examples declare Descriptor with 254-byte attribute arrays, which is why the nRF
port declares it with an empty attribute list instead (design spec fact 8).

#### How the generation actually happens, and why there are two of them

The extension ships `matter_zap_custom_generation`, a component with no
generator of its own, and the obvious reading is that a custom data model opts
into it. **That reading is wrong and the build proves it.** What runs ZAP is
slc-cli itself, for any project carrying a `config_file:` with
`file_id: zap_config`, through the ZAP adapter pack it finds at
`STUDIO_ADAPTER_PACK_PATH`; the stock `lighting-app`, which does not list the
component, gets a full `autogen/zap-generated/` tree all the same. What
`matter_zap_custom_generation` does is flip the `unless:` conditions inside
`matter_static_generated_zap_content`, **removing** the pre-generated
`third_party/matter_sdk/zzz_generated/app-common` include path. Generated with
the component, `grep -c zzz_generated hearth.project.mak` is 0; without it, 1.
That include path is where the per-cluster headers live (`clusters/OnOff/*.h`
and 140 more, which the SDK's own cluster servers include), and a generation run
does not produce them: 28 files out, `clusters/shared` only, the same 28 the
stock light's run produces. The component is for a project that adds a CUSTOM
CLUSTER XML, which is what the one app using it, `performance-test-app`, does.
Hearth's data model is custom but its clusters are all standard, so the
component would take headers away and put none back. It is not in the project.

That leaves two runs of the same generator over the same input: `zap-regen.sh`
into the repository, and slc into the build directory. They were compared file
by file on 2026-09-18 and agree on 27 of 28 files. The 28th is `access.h`, where
the build's copy carries three extra privilege rows for
`BasicInformation::LocalConfigDisabled`, an attribute this data model does not
declare and cannot reach. The cause is zap's persistent sqlite state directory,
which on this bench is the SLT `zap` package itself, because `toolchain.env`
exports `ZAP_DIR` for slt and zap reads `ZAP_DIR` as its own state directory: a
state database with other ZCL packages loaded emits those rows, a fresh one does
not, verified with `--tempState` and with a fresh `--stateDirectory` both. So
`zap-regen.sh` takes a fresh state directory, and the committed tree is the one
that regenerates identically on any machine.

### SDK patches

`sdk-patches/README.md` is the mechanism, the patch, and how a reader tells
whether a tree is patched. The short version: the extension is copied out of the
Conan cache by `fw/sdk-prepare.sh`, which applies
`sdk-patches/matter_sdk/*.patch` after checking each against its `.sha256`, and
stamps the copy with `HEARTH_EEM_PATCH_REV`; `toolchain.env` returns 1 naming
the prepare script if that stamp is missing or at the wrong revision. It is a
revision check and not a presence check because the patch defaults to stock
behaviour when its macro is unset, so a stale cut of it looks exactly like a
current one.

The one patch caps `ElectricalEnergyMeasurement`'s `gMeasurements` table at
`CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES` (8, in
`src/CHIPProjectConfig.h`) instead of the whole dynamic endpoint space. The
cluster is not in this round's build, so the reclaim is not measured here.

### The OpenThread override

`config/sl_openthread_features_config.h` is the SDK's own
`openthread/config/sl_openthread_features_config.h` with **one line changed**:
`OPENTHREAD_CONFIG_MLE_MAX_CHILDREN` is 16 rather than the SDK's 10, so the two
Thread ports of this product answer the same (the nRF arm runs 16, ruling DE412:
the router role is a kept product capability and the mesh sizing is stated
rather than inherited). `hearth.slcp` binds it with a `config_file:`/`override:`
stanza naming `ot_stack_ftd`, which is the component owning that `file_id`
(`ot_stack_mtd` and `ot_stack_rcp` declare the same one and are not in this
build). A project's own `config/` directory is not picked up by slc on its own;
the same stanza shape is what binds the bootloader project's two headers.

It is a frozen copy of a 444-line SDK file, and that is its cost: **on an SDK
bump, diff it against the SDK's** and carry any new options across.

```bash
diff "$SISDK_ROOT/openthread/config/sl_openthread_features_config.h" \
     platform/silabs/config/sl_openthread_features_config.h
```

Today that diff is the changed value and the comment above it, nothing else.

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

### Boot flow: what starts the Matter stack, and in what order

Added round 2 task 3, 2026-09-18. Everything below runs before the AT link
carries its first byte.

| Step | Where | Notes |
|---|---|---|
| `hearth_console_init()` | `app_init_early()`, `src/main.cpp` | USART0 TX on PA00. Before the kernel; see "Migrating to sl_main" |
| `hearth_matter_log_route_init()` | `app_init_early()` | CHIP's log output onto that console, below |
| SDK second stage | `sl_main_second_stage_init()` | `sl_platform_init`, `sl_driver_init`, `sl_service_init`, `sl_stack_init` (the Bluetooth stack starts here), `sl_internal_app_init` (OpenThread's instance, and with it `chip::Platform::MemoryInit()`) |
| `xTaskCreate(hearth_boot_task, ...)` | `app_init()` | 1,280 words, **`osPriorityRealtime7 - 1`**; both numbers are the sample's and both are load-bearing, below |
| `hearth_matter_init("Hearth")` | `port/hearth_matter_init.cpp` | the bring-up, below |
| `emberAfEndpointEnableDisable(240, false)` | `hearth_boot_task`, under `StackLock` | the catalogue endpoint stops being visible. It has to be after `Server::Init`, because the ember tables exist only then |
| `rebuild_composition()` | `hearth_boot_task` | empty until the upward port lands. It is called from where it has to be called from: after `Server::Init`, before the marker |
| `mt_at_start()` | `hearth_boot_task` | emits `+MTREADY` and starts the AT parser task |
| `vTaskDelete(NULL)` | `hearth_boot_task` | the task and its 5 KiB go back to the heap |

#### `hearth_matter_init()`, and what it keeps from the sample

There is no `SilabsMatterConfig::InitMatter()` in this image: that lives in
`MatterConfig.cpp`, which only `matter_platform_mg` compiles, and this project
does not carry that component ("The components, and the three that could not
stay"). So the bring-up is Hearth's own, written from the sample's, with the
`MatterConfig.cpp` line number beside each step in the source so that an SDK
bump can be diffed against it. In order:

`GetPlatform().Init()` (NVM3 plus the key migrations, the reset cause, and
`silabsInitLog()`), `PlatformMgr().InitChipStack()`,
`ConnectivityMgr().SetBLEDeviceName()`, `Provision::Manager::Init()` and the
device-instance-info and commissionable-data providers, `InitOpenThread()`
(`ThreadStackMgr().InitThreadStack()`, `SetThreadDeviceType(Router)`, the
Network Commissioning driver's `Init()`, `ThreadStackMgrImpl().StartThreadTask()`),
the OpenThread endpoint native params (the instance pointer and the two lock
callbacks CHIP's Inet layer needs), then under the stack lock: the report
scheduler, the `Efr32PsaOperationalKeystore`,
`InitializeStaticResourcesBeforeServerInit()`, the codegen data model provider,
the device info provider, and `Server::GetInstance().Init()`. Then
`PlatformMgr().StartEventLoopTask()` and, last, the device attestation
credentials provider, which is where the sample sets it too.

Three steps of the sample are deliberately absent:

- **`chip::Platform::MemoryInit()` is not called.** The sample calls it only
  under `SL_WIFI`; on a Thread build `sl_ot_create_instance()` has already
  called it (`ThreadStackManagerImpl.cpp:184`) from `sl_ot_rtos_stack_init()`,
  which `sl_main_second_stage_init()` runs before `app_init()`. A second call
  is not idempotent: `CHIPMem-Platform.cpp:82-87` `abort()`s on it.
- **`GetPlatform().VerifyIfUpdated()`** clears an NVM3 key only the Matter OTA
  image processor writes, and this product's update story is host-driven serial
  flashing (`FIRMWARE_UPDATE_SPEC.md`), so the call could only ever be a no-op.
- **`AppTask::GetAppTask().StartAppTask()`**, `BaseApplication::sAppDelegate`,
  the shell, the LCD, RPC, tracing, ICD and the Wi-Fi arms: the sample's
  application layer, which this product does not have.

#### The boot task's priority is why BLE advertises

`sl_main` creates its start task at `osPriorityRealtime7`, "the highest
priority" (`sl_main_kernel.c:74-88`), and `SL_MAIN_ENABLE_START_TASK_PRIORITY_CHANGE`
is 0, so it keeps that priority through `app_init()`. The Silicon Labs sample
creates its own bootstrap thread at the same `osPriorityRealtime7`
(`kMainTaskAttr`, `MatterConfig.cpp:194-200`). That attribute is not decoration,
and task 3 found out the hard way what it is for.

With the boot task at `tskIDLE_PRIORITY + 1`, which is where round 1 left it,
the whole stack came up and the device never advertised. The console said why,
once the CHIP log route was installed early enough to catch it:

```
I chip: [DL] Bluetooth stack booted: v11.0.2-b0
E chip: [DL] Failed to schedule work: 1c
... twenty lines later ...
I chip: [DL] Init CHIP Stack
```

The Bluetooth event handler task runs at `SL_BT_RTOS_EVENT_HANDLER_TASK_PRIORITY`
50 (FreeRTOS 49), so a boot task below it is scheduled only after the Bluetooth
stack's boot event has been dispatched.
`BLEManagerImpl::HandleBootEvent()` (`BLEManagerImpl.cpp:785-789`) sets
`Flags::kSiLabsBLEStackInitialize` and then fails to `ScheduleWork`, because the
platform manager does not exist yet; `BLEManagerImpl::_Init()` runs later inside
`InitChipStack()` and its `mFlags.ClearAll()` (`:155`) throws the flag away. No
second boot event ever arrives, so `DriveBLEState()` returns at its first line
(`:417`) for the life of the image.

At the sample's priority the bring-up finishes before the Bluetooth event
handler is scheduled, the flag arrives after `_Init` rather than before it, and
the advertisement goes up. The cost is that the console's blocking writes (about
10 ms a line at 115200) hold the CPU against every lower-priority task for the
~350 ms the init log takes. That is a boot-time cost on a task that deletes
itself a few instructions later.

#### CHIP's log output is on the console now

Round 2 task 2 recorded that CHIP logs reach a SEGGER RTT buffer and neither
UART. Task 3 routed them to Hearth's console instead, with
`chip::Logging::SetLogRedirectCallback()` (`TextOnlyLogging.cpp:121-124`), which
`TextOnlyLogging.cpp:157-168` checks before calling the platform's `LogV`. That
needs neither `matter_uart` (the reason `SILABS_LOG_OUT_UART` is not an option
here) nor a strong symbol over a component source (`LogV` in
`src/platform/silabs/Logging.cpp:227` is a plain definition and cannot be
overridden).

The route is installed in `app_init_early()`, right after the console, not
inside the bring-up: the SDK's Bluetooth and OpenThread tasks log through CHIP
from the moment the kernel starts, and those lines are exactly the ones a
boot-order question needs. Installed later, they are lost, because the RTT sink
is silent until `silabsInitLog()`, which `GetPlatform().Init()` calls from inside
the bring-up.

**Two sinks are not captured** and still go to RTT: `silabsLog()` / `SILABS_LOG`
(`Logging.cpp:175`) and `otPlatLog()` (`:309`), both of which call `PrintLog`
directly. Nothing in this image uses the first; OpenThread's own logging is the
second.

`HEARTH_CHIP_LOG_TO_CONSOLE 0` in `port/hearth_matter_init.cpp` puts CHIP's logs
back on RTT alone. That switch exists because `hearth_log_write()` blocks the
calling task until the last byte is out of the USART
(`port/hearth_port_sl.c`), and the calling task is usually the CHIP event loop.
A round that finds the event loop starved under commissioning traffic turns this
off first and measures second.

#### The fault hooks are strong symbols now

`src/sdk/SoftwareFaultReports.cpp` is a verbatim copy of the extension's
`examples/platform/silabs/SoftwareFaultReports.cpp`, compiled by `hearth.slcp`.
Copied rather than referenced because `slc` resolves a project's `source:` paths
relative to the `.slcp` and there is no portable relative path from this
repository to `$MATTER_EXT_ROOT`; `src/sdk/README.md` carries the origin, the
staleness check and why `silabs_utils.cpp` did not come with it. Proven in the
linked image:

```
$ arm-none-eabi-nm -C build/debug/hearth.out | grep -E " (vApplication|HardFault|BusFault|UsageFault|RAILCb|debugHardfault)"
08052bc8 T BusFault_Handler
08052a7c T debugHardfault
08052ba0 T HardFault_Handler
08052ccc T RAILCb_AssertFailed
08052bdc T UsageFault_Handler
08052c9c T vApplicationGetIdleTaskMemory
0806e54e W vApplicationIdleHook
08052c18 T vApplicationMallocFailedHook
08052c4c T vApplicationStackOverflowHook
```

`vApplicationIdleHook` stays weak on purpose: the sample's implementation lives
in `MatterConfig.cpp`, which this project does not compile, and its whole body is
behind `SLI_SI91X`, ICD and watchdog conditions that are all off here.

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

Four sets of figures live here. **"Round 2 task 3" immediately below is the
current image**, the first one with the Matter stack running. "Round 2 task 2"
after it is the same image with the stack linked but not started, which is what
task 3's figures are measured against; "Round 2 baseline" is the skeleton on
`sl_main` with no Matter at all; and everything after that is round 1's
`sl_system` image, kept because the Phase 1 result set, the 35 failing rows and
the bench facts are still the record round 2 works from, and because a figure is
only worth what its provenance says.

The round 1 figures: the skeleton image, 2026-09-17, built from the committed
tree at `233778c` in `~/silabs/work/hearth-skeleton` by the "Building" recipe
above (which now names round 2's directory), running on the MGM240PA32VNA3 on
the iLabs RP2350 carrier.

The image was built and flashed twice, from `19e7fe8` and then from `233778c`
(a comment-only change), each from a clean `slc generate`. **The figures below
are the second build's**, with one exception that is a genuine comparison: the
Phase 1 result set was captured on both runs and diffed row for row, and the
two are identical (see "Harness Phase 0 and Phase 1"). The two build logs
differ only in the line number of a deprecation warning, which is the comment
that moved. Nothing else below is a two-run figure, and nothing else below
claims to be.

### Round 2 task 3: the Matter stack started

Built 2026-09-18 from the committed tree at `21b0b8c` in
`~/silabs/work/hearth-matter-t3` by the "Building" recipe above (a clean
`slc generate` into an empty directory), flashed with `fw/flash.py` and run on
the MGM240PA32VNA3 on the iLabs RP2350 carrier. `hearth.bin`
`md5sum 4fa64cbbc44eaadc9349fdc98daceb94`.

This image initialises `chip::Server`, runs the CHIP event loop and advertises
over BLE. The upward port is still `port/mt_matter_stub.c`, so the AT surface
answers exactly as it did.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t3 \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t3/build.log
$ grep -c deprecated ~/silabs/work/hearth-matter-t3/build.log
0
$ grep -ci warning ~/silabs/work/hearth-matter-t3/build.log
0
```

`arm-none-eabi-size ~/silabs/work/hearth-matter-t3/build/debug/hearth.out`, with
task 2's figures beside it:

| | Task 3, bytes | Task 2, bytes | Delta |
|---|---|---|---|
| `text` | 824 560 | 728 200 | +96 360 |
| `data` | 3 352 | 3 116 | +236 |
| `bss` (size's, includes the heap section) | 258 340 | 258 576 | -236 |

`arm-none-eabi-size -A` on the same file:

| Section | Task 3, bytes | Task 2 | Where |
|---|---|---|---|
| `.text` | 823 724 | 727 364 | flash |
| `.vectors` | 368 | 368 | flash, at 0x08006000 |
| `.ARM.exidx` | 8 | 8 | flash |
| `.copy.table` | 12 | 12 | flash |
| `.zero.table` | 0 | 0 | flash |
| `.data` | 3 352 | 3 116 | RAM, loaded from flash |
| `.nvm` | 40 960 | 40 960 | flash, at the top of the application region |
| `.bss` | 114 104 | 110 264 | RAM |
| `.memory_manager_heap` | 139 624 | 143 700 | RAM, whatever is left |
| `.stack` | 4 608 | 4 608 | RAM, the C stack |
| `text_application_ram` | 448 | 448 | RAM |
| `.bootloader_reset_section` | 4 | 4 | RAM, at 0x20000000 |

The RAM rows sum to 262 140 B, the same 4 B of alignment padding below the
part's 262 144 that round 1's section explains. **Calling into the stack costs
96,360 B of flash** over linking it, which is the code the linker could discard
while nothing reached it: `Server`, the interaction model, the codegen data
model provider, the BLE manager's advertising path and the OpenThread
network-commissioning driver. `.bss` grows 3,840 B and the memory-manager heap
gives that back.

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus  827928 Sep 18 17:52 hearth.bin
-rw-rw-r-- 1 pontus pontus  828000 Sep 18 17:52 hearth.gbl
-rwxrwxr-x 1 pontus pontus 2483832 Sep 18 17:52 hearth.s37
```

The application image on flash is **827 928 B**, 53.76 % of the 1 540 096 B
application region, against task 2's 731 332 B and 47.49 %. The stock Silicon
Labs `lighting-app` for this part was 1 025 140 B with the shell, the OTA
requestor, scenes, level and colour control; the nRF54L15 core round shipped at
753 691 B. This image is now about 74 KB above the nRF's, which is the first
figure in this round where it is, and it carries a catalogue endpoint's worth of
cluster servers that no composition serves yet.

`hearth.gbl` is **828 000 B**, which `fw/flash.py` sent as 6 469 blocks in
91.8 s with no retransmits (task 2: 5 715 blocks in 81.0 s).

#### Free heap at `+MTREADY`, task 3

**98 968 B free**, logged by `sl_memory_get_free_heap_size()` immediately after
`mt_at_start()` returns, out of the 139 624 B `.memory_manager_heap`. Task 2's
figure at the same point was 104 784 B, so the running stack has taken about
5.8 KB of heap by the time the marker goes out.

**The measurement point matters more than it used to, and this figure is not the
steady state.** The boot task runs at `osPriorityRealtime7`, so it logs this line
before the Bluetooth event handler task is ever scheduled: the Bluetooth stack's
boot-time allocations and the advertising set are NOT counted. The same image
built with the boot task at `tskIDLE_PRIORITY + 1` (the version that did not
advertise, "The boot task's priority is why BLE advertises") logged 96 944 B at
the same line with those allocations already made, so the difference is about
2 KB. As before, the figure also still counts the boot task's own 5 KiB stack
and TCB, which are released a few instructions later.

#### The boot log and `+MTREADY`, task 3

Captured 2026-09-18, both ports at once with timestamps: the console on the
Debug Probe's UART CDC at 115200 with DTR asserted, the AT link on the carrier's
CDC with DTR and RTS cleared before the open, whose own reset is the reset being
watched.

```
AT in full: b'+MTREADY\r\n', 10 bytes        (first byte at 0.340 s)
```

**Nothing before it and nothing else**, with the whole Matter stack coming up in
between. The marker arrives at 0.340 s against task 2's 0.151 s; the extra
~190 ms is `hearth_matter_init()`, most of it the console printing the stack's
own init log at 115200.

The console, in full:

```
I boot: Hearth on USART0 TX PA00, 115200 8N1 (sl_main)
I boot: boot task up, model MGM240P Hearth
I chip: [DL] Init CHIP Stack
I chip: [DL] Setting device name to : "Hearth"
I chip: [DL] Provision mode disabled
I chip: [DL] Initializing OpenThread stack
I chip: [DL] OpenThread started: OK
I chip: [DL] Setting OpenThread device type to ROUTER
I chip: [DL] Starting OpenThread task
I chip: [SVR] Initializing subscription resumption storage...
I chip: [SVR] Server initializing...
I chip: [TS] Last Known Good Time: 2023-10-10T16:28:52
I chip: [DMG] AccessControl: initializing
I chip: [DMG] Examples::AccessControlDelegate::Init
I chip: [DMG] AccessControl: setting
I chip: [DMG] DefaultAclStorage: initializing
I chip: [DMG] DefaultAclStorage: 0 entries loaded
I chip: [SVR] WARNING: mTestEventTriggerDelegate is null
I chip: [ZCL] Using ZAP configuration...
I chip: [ZCL] Endpoint f0 On/off already set to new value
I chip: [DMG] AccessControlCluster: initializing
I chip: [DIS] Updating services using commissioning mode 1
E chip: [DIS] Failed to remove advertised services: 3
I chip: [DIS] Advertise commission parameter vendorID=65521 productID=32784 discriminator=3840/15 cm=1 cp=0 jf=0
E chip: [DIS] Failed to advertise commissionable node: 3
E chip: [DIS] Failed to finalize service update: 3
I chip: [DIS] Updating services using commissioning mode 1
E chip: [DIS] Failed to remove advertised services: 3
I chip: [DIS] Advertise commission parameter vendorID=65521 productID=32784 discriminator=3840/15 cm=1 cp=0 jf=0
E chip: [DIS] Failed to advertise commissionable node: 3
E chip: [DIS] Failed to finalize service update: 3
I chip: [IN] CASE Server enabling CASE session setups
I chip: [SVR] Server Listening...
I matter: stack up: server initialised, event loop running
I chip: [ZCL] Shuting down on/off server cluster on endpoint 240
I at_parser: parser started
I boot: +MTREADY sent, free heap 98968 B
I chip: [DL] Bluetooth stack booted: v11.0.2-b0
I chip: [DL] RAIL version:, v3.0.3-b0
I chip: [DL] BLE Static Device Address CC:0F:28:A4:A2:69
I chip: [DL] Starting advertising with interval_min=32, intverval_max=96 (units of 625us)
I chip: [DL] _OnPlatformEvent default:  event->Type = 32781
I chip: [DL] _OnPlatformEvent default:  event->Type = 32779
```

Three lines in that log are expected and are not defects:

- **`Shuting down on/off server cluster on endpoint 240`** (the SDK's spelling)
  is the catalogue endpoint being disabled, which is the port doing its job.
- **`Failed to advertise commissionable node: 3`**, three times over, is DNS-SD.
  The commissionable advertisement goes out over Thread through the SRP client,
  and this device has no dataset and no network, so there is no SRP server to
  register with. BLE is the commissioning path until then, and it is up.
- **`WARNING: mTestEventTriggerDelegate is null`** is the sample's
  `SL_MATTER_TEST_EVENT_TRIGGER_ENABLED` block, which this project does not
  define and which exists for certification test harnesses.

#### BLE, task 3

The liveness proof this task owes, `bluetoothctl` on the dev box, 2026-09-18
(commissioning itself belongs to a later task):

```
$ bluetoothctl     # scan le; the module was reset by the capture above
[NEW] Device CC:0F:28:A4:A2:69 Hearth

$ bluetoothctl info CC:0F:28:A4:A2:69
Device CC:0F:28:A4:A2:69 (random)
	Name: Hearth
	Alias: Hearth
	UUID: Unknown                   (0000fff6-0000-1000-8000-00805f9b34fb)
	ServiceData.0000fff6-0000-1000-8000-00805f9b34fb:
  00 00 0f f1 ff 10 80 00
```

`scan le` matters; a plain `scan on` reports nothing here, exactly as it did for
the stock example. The address is the one the console printed as the BLE static
device address on that boot, and it is regenerated at every boot by design
(`BLEManagerImpl::_Init`). The service data decodes to Hearth's own identity:
commissioning flag `00`, discriminator `0x0F00` = 3840, vendor `0xFFF1`, product
`0x8010`, which is `CHIPProjectConfig.h` read back off the air.

#### Harness Phase 0 and Phase 1, task 3

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
===== RESULT: 0 passed, 0 failed =====

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico \
      --transport THREAD --phase 1
===== RESULT: 261 passed, 35 failed =====
```

The run was redirected to a file and compared against
`platform/silabs/skeleton-phase1.json` **name by name, not count by count**: 296
rows on both sides, no row present on one side only, and **zero verdict
differences**. The same 261 pass and the same 35 fail. The baseline file is not
rewritten.

Host gates the same day, on the same tree: `make -C test/host run` green (zero
`[FAIL]` lines; `check_decls.py` 61/61 + 4/4 + 24/24 + 1/1 and
`check_slcp_sources.py` silent), `python3 test/test_mt_regression.py` ran 467
tests, OK, and `fw/zap-regen.sh --check` reported the committed tree current.

### Round 2 task 2: the Matter stack linked, not started

Built 2026-09-18 from the committed tree at `4c0f989` in
`~/silabs/work/hearth-matter` by the "Building" recipe above (a clean
`slc generate` into an empty directory), flashed with `fw/flash.py` and run on
the MGM240PA32VNA3 on the iLabs RP2350 carrier. `hearth.bin`
`md5sum 27ee249b15246bc8e31b07f450c9cd94`.

Nothing in this image calls into the stack: `Server::Init` is never reached, the
upward port is still `port/mt_matter_stub.c`, and the figures below are what the
stack costs by being LINKED. They are the baseline task 3's "started" figures
are compared against.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter/build.log
$ grep -c deprecated ~/silabs/work/hearth-matter/build.log
0
$ grep -ci warning ~/silabs/work/hearth-matter/build.log
0
```

`arm-none-eabi-size ~/silabs/work/hearth-matter/build/debug/hearth.out`, with
the round 2 baseline beside it:

| | Task 2, bytes | Round 2 baseline, bytes | Delta |
|---|---|---|---|
| `text` | 728 200 | 48 336 | +679 864 |
| `data` | 3 116 | 176 | +2 940 |
| `bss` | 258 576 | 261 536 | -2 960 |

`bss` goes DOWN because `arm-none-eabi-size` counts `.memory_manager_heap` in
it, and that section is whatever RAM is left over: the stack's static RAM came
out of the heap, not out of the part. The `-A` split is where the real movement
is.

`arm-none-eabi-size -A` on the same file:

| Section | Bytes | Round 2 baseline | Where |
|---|---|---|---|
| `.text` | 727 364 | 47 520 | flash |
| `.vectors` | 368 | 368 | flash, at 0x08006000 |
| `.ARM.exidx` | 8 | 8 | flash |
| `.copy.table` | 12 | 12 | flash |
| `.zero.table` | 0 | 0 | flash |
| `.data` | 3 116 | 176 | RAM, loaded from flash |
| `.nvm` | 40 960 | 40 960 | flash, at the top of the application region |
| `.bss` | 110 264 | 42 928 | RAM |
| `.memory_manager_heap` | 143 700 | 217 580 | RAM, whatever is left |
| `.stack` | 4 608 | 1 024 | RAM, the C stack; raised by `SL_STACK_SIZE` |
| `text_application_ram` | 448 | 428 | RAM |
| `.bootloader_reset_section` | 4 | 4 | RAM, at 0x20000000 |

The RAM rows sum to 262 140 B again, the same 4 B of alignment padding below the
part's 262 144 that round 1's section explains. **The stack costs 67,336 B of
`.bss` and 2,940 B of `.data` before it is ever started**, and the
memory-manager heap gives up 73,880 B to pay for that plus the bigger C stack.

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus  731332 Sep 18 16:41 hearth.bin
-rw-rw-r-- 1 pontus pontus  731404 Sep 18 16:41 hearth.gbl
-rwxrwxr-x 1 pontus pontus 2194048 Sep 18 16:41 hearth.s37
```

The application image on flash is **731 332 B**, 47.49 % of the 1 540 096 B
application region, against the skeleton's 48 516 B and 3.15 %. For scale, the
stock Silicon Labs `lighting-app` for this part was 1 025 140 B with the shell,
the OTA requestor, scenes, level and colour control compiled in, and the
nRF54L15 core round shipped at 753,691 B of flash. That is the yardstick the
design spec asks this figure to be read against, and this image is about 22 KB
below it. The two are not built the same way and the comparison is an order of
magnitude rather than a like-for-like difference; what it settles is the
question the spec actually asks, whether this image is materially ABOVE the
nRF's for the same coverage. It is not.

`hearth.gbl` is **731 404 B**, which `fw/flash.py` sent as 5 715 blocks in
81.0 s with no retransmits (the skeleton was 374 blocks in 5.4 s; the bootloader
runs about 14 ms per 128-byte block, so the transfer time is the image size).

#### Free heap at `+MTREADY`, task 2

**104 784 B free**, logged by `sl_memory_get_free_heap_size()` immediately after
`mt_at_start()` returns, out of the 143 700 B `.memory_manager_heap`.

This is NOT comparable with round 1's 13 768 B or task 1's 9 560 B: those were
`xPortGetFreeHeapSize()` over a separate 24 576 B FreeRTOS pool that no longer
exists (see "The heap changed shape"). The pools are one now, and this figure
counts everything the kernel and the SDK have taken, including the boot task's
own 4 KiB stack and TCB, which are released a few instructions later. CHIP has
allocated nothing at this point: the stack is not started.

#### The boot log and `+MTREADY`, task 2

Captured 2026-09-18, both ports at once with timestamps: the console on the
Debug Probe's UART CDC at 115200 with DTR asserted, the AT link on the carrier's
CDC with DTR and RTS cleared before the open, whose own reset is the reset being
watched.

```
  0.010  CONSOLE b'\x00'
  0.091  CONSOLE b'I boot: Hearth on USART0 TX PA00, 115200 8N1 (sl_main)\r\n'
  0.151  CONSOLE b'I boot: boot task up, model MGM240P Hearth\r\nI at_parser: parser started\r\n'
  0.151  AT      b'+MTR'
  0.161  CONSOLE b'I boot: +MTREADY sent, free heap 104784 B\r\n'
  0.161  AT      b'EADY\r\n'
```

AT link, in full: `b'+MTREADY\r\n'`, 10 bytes, nothing before it and nothing
else. **The boot contract is unchanged by linking the stack in**, and this
capture is the evidence: the same three console lines, in the same order, before
the first byte of the marker, and no URC precedes it. The marker arrives at
0.151 s against the baseline's 0.102 s; the extra ~50 ms is the SDK's second
stage bringing up the larger image.

#### Harness Phase 0 and Phase 1, task 2

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
===== RESULT: 0 passed, 0 failed =====

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico \
      --transport THREAD --phase 1
===== RESULT: 261 passed, 35 failed =====
```

The run was redirected to a file and compared against
`platform/silabs/skeleton-phase1.json` **name by name, not count by count**: 296
rows on both sides, no row present on one side only, and **zero verdict
differences**. The same 261 pass and the same 35 fail. The baseline file is not
rewritten, because nothing it records changed; the 35 rows below are still this
round's checklist, and they stay failing until the stack is started and the
stubs are retired.

Host gates the same day, on the same tree: `make -C test/host run` green
(including `check_decls.py` 61/61 + 4/4 + 24/24 + 1/1 and
`check_slcp_sources.py`), and `python3 test/test_mt_regression.py` ran 467 tests,
OK.

### Round 2 baseline: the same skeleton on `sl_main`

Built 2026-09-18 from the committed tree at `188dee5` in
`~/silabs/work/hearth-core` by the "Building" recipe above, flashed with
`fw/flash.py` and run on the same carrier. No Matter component yet: this is the
round 1 skeleton with the entry point moved and the four round 1 deferrals
closed, measured once so the stack has a clean baseline to be compared against.

`6a52f84`, a later comment-only fix to `src/main.cpp`, was rebuilt from a clean
`slc generate` and produces a byte-identical `hearth.bin` (`md5sum`
`4b311f1cb4f1b66e2ef7eefb54da6048` from both trees), so every figure here still
belongs to the build it names.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-core \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-core/build.log
$ grep -c deprecated ~/silabs/work/hearth-core/build.log
0
$ grep -ci warning ~/silabs/work/hearth-core/build.log
0
```

`arm-none-eabi-size ~/silabs/work/hearth-core/build/debug/hearth.out`, with
round 1's figures beside them:

| | Round 2, bytes | Round 1, bytes | Delta |
|---|---|---|---|
| `text` | 48 336 | 47 596 | +740 |
| `data` | 176 | 176 | 0 |
| `bss` | 261 536 | 261 536 | 0 |

`arm-none-eabi-size -A` on the same file:

| Section | Bytes | Where |
|---|---|---|
| `.text` | 47 520 | flash |
| `.vectors` | 368 | flash, at 0x08006000 |
| `.ARM.exidx` | 8 | flash; new, the C++ unwind index `main.cpp` brings |
| `.copy.table` | 12 | flash; new with `sl_main` |
| `.zero.table` | 0 | flash; new with `sl_main` |
| `.data` | 176 | RAM, loaded from flash |
| `.nvm` | 40 960 | flash, at the top of the application region; NVM3's store, not code |
| `.bss` | 42 928 | RAM |
| `.memory_manager_heap` | 217 580 | RAM, the SDK memory manager taking whatever is left |
| `.stack` | 1 024 | RAM, the C stack; **1 KiB now, 4 KiB in round 1**, see below |
| `text_application_ram` | 428 | RAM |
| `.bootloader_reset_section` | 4 | RAM, at 0x20000000 |

The RAM rows sum to 262 140 B again, the same 4 B of alignment padding below
the part's 262 144 that round 1's section explains.

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus  48516 Sep 18 15:39 hearth.bin
-rw-rw-r-- 1 pontus pontus  48600 Sep 18 15:39 hearth.gbl
-rwxrwxr-x 1 pontus pontus 145624 Sep 18 15:39 hearth.s37
```

The application image on flash is **48 516 B**, 3.15 % of the 1 540 096 B
application region; `hearth.gbl` is **48 600 B**, which `fw/flash.py` sent as
380 blocks in 5.4 s with no retransmits. The move costs **+740 B of flash**,
the same +740 on `text`, `hearth.bin` and `hearth.gbl`: the SDK's `main.c`,
`main_retarget.c` and the C++ unwind tables, minus the two `sl_system` calls.

**The C stack dropped from 4 096 to 1 024 B, and Hearth did not do it.**
`memory_manager_region.slcc` picks a different config file per entry point:
`config/legacy/sl_memory_manager_region_config.h` (`SL_STACK_SIZE 4096`)
`unless: [sl_main]`, and `config/rtos/sl_memory_manager_region_config.h`
(`SL_STACK_SIZE 1024`) `condition: [kernel, sl_main]`. Under `sl_main` with a
kernel that stack carries only `sl_main_init()` before the scheduler, the two
app hooks' early half, and every ISR; each task has its own stack. It is the
SDK's choice for this configuration and is left at the SDK's value, but it is
a number the Matter stack's interrupt handlers will share, so the first task
that adds one should know it moved. `SL_STACK_SIZE` in the `.slcp`'s
`configuration:` is the one line that raises it.

The checked `xTaskCreate()` was proven by injecting the failure, 2026-09-18:
the boot task's stack request was temporarily raised to 60 000 words, which
`configTOTAL_HEAP_SIZE` cannot satisfy, and that image was built, flashed and
watched on both ports. The console said so and the AT link stayed silent:

```
  0.062  CONSOLE b'I boot: Hearth on USART0 TX PA00, 11520'
  0.072  CONSOLE b'0 8N1 (sl_main)\r\n'
  0.092  CONSOLE b'E boot: boot task could not be created (FreeRTOS heap 20272 B free)\r\n'

AT in full: b'', 0 bytes
```

`fw/flash.py` exited 1 on that image with "no +MTREADY within 20 s after the
upload", which is the other half of the contract: a boot that fails is a flash
that fails, not a flash that passes quietly. The tree was restored and the
measured image reflashed immediately afterwards; the injected build is not a
commit and its directory was deleted. Round 1's version of this path logged
nothing on either UART, which is why the row existed.

### Free FreeRTOS heap at `+MTREADY`, round 2

`configTOTAL_HEAP_SIZE` is unchanged at 24 576. `xPortGetFreeHeapSize()`
logged immediately after `mt_at_start()` returns: **9 560 B free**, against
round 1's 13 768 B. The 4 208 B difference is `sl_main`'s start task, which is
what calls `app_init()`: `SL_MAIN_START_TASK_STACK_SIZE_BYTES` is 4 096 and its
TCB is the rest. That task ends as soon as `app_init()` returns and the idle
task reclaims it, exactly as the boot task's own 4 KiB is reclaimed a few
instructions after this line is logged, so the steady-state figure is about
8 KiB above the logged one. Neither number is the steady state; both are the
same measurement point as round 1's, which is what makes them comparable.

### The boot log and `+MTREADY`, round 2

Captured 2026-09-18, both ports at once with timestamps: the console on the
Debug Probe's UART CDC at 115200 with DTR asserted, the AT link on the
carrier's CDC with DTR and RTS cleared before the open, whose own reset is the
reset being watched.

```
  0.003  CONSOLE b'\x00'
  0.064  CONSOLE b'I boot: Hearth on USART0 TX PA00, 115200 8N1 (sl_main)\r'
  0.074  CONSOLE b'\n'
  0.094  CONSOLE b'I boot: boot task up, model MGM240P Hearth\r\n'
  0.102  AT      b'+MTREADY\r\n'
  0.104  CONSOLE b'I at_parser: parser started\r\nI boot: +MTREADY sent, free FreeRTOS heap 9560 B\r\n'
```

AT link, in full: `b'+MTREADY\r\n'`, 10 bytes, nothing before it and nothing
else. The boot contract is unchanged by the migration, and this capture is the
evidence: the first two console lines are on the wire before the first byte of
the marker, and no URC precedes it.

### Harness Phase 0 and Phase 1, round 2

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
===== RESULT: 0 passed, 0 failed =====

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico \
      --transport THREAD --phase 1
===== RESULT: 261 passed, 35 failed =====
```

The run was redirected to a file and diffed against
`platform/silabs/skeleton-phase1.json` name by name, not count by count: the
same 261 rows pass and the same 35 fail, with no row moving in either
direction. The baseline file is **not** rewritten, because nothing it records
changed; the 35 rows below are still this round's checklist.

### Image size, round 1

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

### Free FreeRTOS heap at `+MTREADY`, round 1

`configTOTAL_HEAP_SIZE` is **24 576** (raised from the SDK default of 8 192,
which does not hold the boot task and the 6 KiB AT parser task at the same
time). `xPortGetFreeHeapSize()` logged immediately after `mt_at_start()`
returns: **13 768 B free**. That figure still counts the boot task's own 4 KiB
stack and TCB, which are released a few instructions later by
`vTaskDelete(NULL)`, so the steady-state figure is about 4 KiB higher. This is
the FreeRTOS heap only; `.memory_manager_heap` is a separate 214 512 B pool
that nothing in the skeleton allocates from.

### The boot log, and `+MTREADY` after it, round 1

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

### Harness Phase 0 and Phase 1, round 1

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

Round 2 Task 2 (2026-09-18) took the static ZAP and the disabled catalogue
endpoint 240 out of the first row below, and added four rows of its own at the
bottom of the table.

Round 2 Task 3 (2026-09-18) closed two of task 2's rows: the two serverless
endpoint 0 clusters are disabled on this arm, and CHIP's log output is on the
console. It added two rows of its own, and the first row below shrank again:
`chip::Server` is started, the event loop runs and the catalogue endpoint is
disabled, so what is left of it is the upward port proper.

| Open item | Owner |
|---|---|
| The upward port: the composition rebuild, the dynamic endpoint machinery, the arenas and the device-type catalogue in audited batches, each batch retiring stubs from `port/mt_matter_stub.c` and `port/mt_devtypes_stub.c`. `chip::Server` is started and the event loop runs since task 3; `rebuild_composition()` in `src/main.cpp` is the empty hook they fill, and it is already called from the one place it can be called from | the **upward-port round** (graph T446). Its starting checklist is "The 35 failing rows" above, and the run they came from is the baseline `platform/silabs/skeleton-phase1.json` |
| Signing (ECDSA-P256 dev key under `keys/`, as the nRF port does), secure boot, and the SE debug lock after the one-time SWD install | **pre-ship** (design spec section 7, stage 2). The bootloader itself is built and installed already; it accepts an unsigned `.gbl` today |
| Thread-arm baselines under `test/baselines/`, ARCHITECTURE 8.21 and its decision-log rows, and the host library's `fw/README` variant table | the **qualification round** (design spec section 8, steps 6 and 7). The skeleton's Phase 1 record deliberately stays out of `test/baselines/`, which holds shipping baselines only |
| The RX ring overflow warning is observable but has never fired: no run so far pushed the link hard enough for its absence to be evidence | the **upward-port round**'s first sustained traffic; see "What is not measured here" |
| The console is TX only by design, so the port has no console input path and no shell | settled, not open: it is board contract item 6 and a CRA posture (`CRA_COMPLIANCE.md` in the docs repository). Listed here so nobody reopens it as an omission |
| `CHIPProjectConfig.h` does not set `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` or its string, so BasicInformation reports the SDK default `1` / `"1.0"` while `AT+CGMR` answers `MT_FW_VERSION`, 1.2.0. Two version surfaces, one of them wrong. A second hand-maintained copy of the version would drift, so the fix is to derive it | the **qualification round**, which is what makes the two surfaces answerable together |
| Diagnostic Logs and Wi-Fi Network Diagnostics are disabled on THIS arm since task 3 and still enabled on the nRF arm's, so the two Thread ports' root nodes no longer answer the same | the **qualification round**, which owns the wire surface for both arms at once; see "Data model" |
| `src/sdk/SoftwareFaultReports.cpp` is a verbatim copy of an extension source, carried because slc cannot reference a file outside the project by a portable relative path. It goes stale silently on an SDK bump, exactly like the OpenThread override | whoever bumps the SDK; the diff command is in `src/sdk/README.md` |
| The Matter BLE advertisement carries the fixed name `Hearth` rather than the SDK's `<prefix><discriminator>` form, because `SetBLEDeviceName()` replaces that form rather than decorating it. Two units on one bench are indistinguishable by name, though on this build they would be anyway: the discriminator is the fixed test value | the **qualification round**; both macros and the reasoning are in `src/CHIPProjectConfig.h` |
| `config/sl_openthread_features_config.h` is a frozen 444-line copy of an SDK file with one value changed. It goes stale silently on an SDK bump | whoever bumps the SDK; the diff command is under "The OpenThread override" |
