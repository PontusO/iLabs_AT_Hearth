# Hearth on Silicon Labs EFR32MG24 (MGM240P)

Status: **round 2 (the Matter core) is complete and bench-accepted on
`dev/silabs-mg24-bringup`, 2026-09-18.** The MGM240PA32VNA3 speaks Matter over
Thread: it was commissioned onto the bench border router's fabric twice in one
session (nodes `0x4901` and, after `AT+MTFRESET`, `0x4902`), a controller
toggles the light and the `+MTATTR` URC reports it on the AT link, an AT-side
write to the temperature sensor reads back from the controller, and harness
Phase 1 is 292/4 while commissioned. Since the `+MTEVT` parity round,
2026-09-21, it also emits the `+MTEVT` URCs ("Events (`+MTEVT`)") and passes
harness Phase 2, the Matter lifecycle chain, at **98/2/1 with every event row
passing** ("Harness Phase 2"). What it does not do yet is on this page:
"What round 2 leaves open" at the end. Round 1's own status, unchanged:

**Round 1 (bring-up, downward port, boot contract) was complete on
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

**Task 4, 2026-09-18, made the first upward-port section real.**
`port/mt_matter_sl.cpp` answers the commissioning state, the fabric count, the
commissioning window, the onboarding codes, the factory reset, the net info,
the transport-mismatch question and the Thread role reads from the running
stack instead of from `port/mt_matter_stub.c`. It is the nRF port's first
section transferred whole ("Port sections" below). Phase 1 moves to **264/32**:
`MTCODES? format`, `MTCODES? stable across reads` and `MTTHREAD? shape by
image` turn green and nothing else changes verdict ("Measured", "Round 2 task
4").

**Task 5, 2026-09-18, made the device-type side real.** `port/mt_devtypes_sl.cpp`
carries the whole 52-row registry, the endpoint block arena, the port-owned
external attribute store and its ember callbacks, and builds two of the
catalogue's device types: the on/off light and the temperature sensor. A stored
composition is rebuilt as dynamic endpoints at boot, before `+MTREADY`, so
`AT+MTEP?` reports what the device is actually serving; the other 50 rows answer
the `AT+MTEP=` gate predicates exactly as the nRF's do and are refused at the
rebuild, which keeps the endpoints before them live. Phase 1 moves to
**267/29**: the three parent-gate rows turn green and nothing else changes
verdict ("Measured", "Round 2 task 5"; "Port sections" for the transfer and the
registry policy).

**Task 6, 2026-09-18, made the attribute bridge real.** `AT+MTATTR` reads and
writes reach the ember attribute store under the CHIP stack lock, and
`MatterPostAttributeChangeCallback` raises the `+MTATTR` URC for every change
that actually changes a value, controller-driven or local, in both notify
modes. Two things came with it. The image formats with the SDK's tiny printf
now, because newlib-nano cannot print a 64-bit integer at all and the first
command to ask it to was this one ("The 64-bit value the libc could not print"
below). And every stub for a family this image has not ported stopped answering
"no such endpoint" for an endpoint that exists: it answers its own family's
"no such cluster on that endpoint" code instead, from one lookup in the live
endpoint table. Phase 1 moves to **292/4** ("Measured", "Round 2 task 6"),
and all four remaining failures are named there with their cause: each stages a
composition of catalogue device types this round does not build.

**Task 7, 2026-09-18, proved the round on the bench and closed its memory
record.** Two commissionings, both control directions, a factory reset between
them, Phase 1 at 292/4 while commissioned, the first burst that ever fired the
RX ring overflow warning, and the flash, RAM, arena and NVM3 figures with their
provenance ("Measured", "Round 2 task 7"; "Commissioning" for the procedure and
the transcripts). One source change came with it, the NVM3 occupancy boot line,
because the bench has no other way to read that figure. Two things the
acceptance could not answer are named rather than passed: the commissioning
`+MTEVT` URCs, which this arm does not emit because the platform event bridge
is unported (graph B502, and the nRF arm is inferred to be in the same
position), and harness Phase 2, which aborts at its own precondition because
the bench's OpenThread control socket is root-owned (graph F503).

**The `+MTEVT` parity round, 2026-09-21, answered both.** The events are
emitted from a file shared with the nRF arm ("Events (`+MTEVT`)"), and harness
Phase 2 now runs here end to end: **98 passed, 2 failed, 1 not applicable**,
every event row passing ("Harness Phase 2"). The two failures are one port
defect, dynamic-endpoint attribute values not surviving a reboot, which the nRF
arm shares by construction and which is listed with its owner below.

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

`~/silabs/work/hearth-matter-t7` is round 2 task 7's build directory and the
one the current image came from. It is also the directory task 7's scratch heap
probe was built and flashed from and then removed again, which the rebuilt
`hearth.bin`'s md5 proves left nothing behind ("Round 2 task 7").
`~/silabs/work/hearth-matter-t6r` is round 2 task 6's tree after its fix round;
`~/silabs/work/hearth-matter-t6` is the same task's pre-fix-round tree, kept
because the tiny printf's own 3,272 B figure is measured against it. Task 5's `~/silabs/work/hearth-matter-t5`,
task 4's `~/silabs/work/hearth-matter-t4`,
task 3's `~/silabs/work/hearth-matter-t3r` (its directory after the review
fix), task 2's `~/silabs/work/hearth-matter`, round 1's
`~/silabs/work/hearth-skeleton` and task 1's `~/silabs/work/hearth-core` are
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
- **Endpoint 240's device type 0x0101 (dimmable light) is left as it is**,
  originally with Level Control disabled under it. It was the same shape of
  inconsistency and deliberately not the same problem: this endpoint is the
  CATALOGUE, the port disables it at runtime before any fabric can read it
  (which is what "disabled catalogue endpoint" means and is task 3's
  `emberAfEndpointEnableDisable` call), and its device type list is never
  served. Removing it would diverge this arm's model from the nRF arm's, which
  carries the same entry, for no wire effect. A known artefact, recorded here
  so nobody has to rediscover it; the round that gives endpoint 240 a device
  type it means is the one that should change it, for both arms.
- **Catalogue batch 1's task 2 (2026-09-22) enables the server side of seven
  clusters bound to endpoint 240's `MA-dimmablelight` endpoint type**: Level
  Control (8), Boolean State (69), Relative Humidity Measurement (1029),
  Pressure Measurement (1027), Illuminance Measurement (1024), Flow
  Measurement (1028) and Occupancy Sensing (1030), with `hearth.slcp` gaining
  the three components that have a cluster-server directory
  (`matter_level_control`, `matter_boolean_state`, `matter_occupancy_sensor`);
  the other four have no component to add, served out of ember attribute
  storage the same way Temperature Measurement already was. This is a data
  model and image-link change only, before the catalogue registry gives any of
  the seven clusters a table: the seven enabled clusters carry no attribute
  data until the next task builds the registry rows that create them.
- **Catalogue batch 2's task 1 (2026-09-24) enables the server side of five
  more clusters bound to endpoint 240's `MA-dimmablelight` endpoint type**:
  Color Control (768), Thermostat (513), Fan Control (514), Window Covering
  (258) and Air Quality (91), with `hearth.slcp` gaining all five
  components, since all five have a cluster-server directory
  (`matter_color_control`, `matter_thermostat`, `matter_fan_control`,
  `matter_window_covering`, `matter_air_quality`), unlike batch 1 where four
  of the seven had none. All five are ember-served on dynamic endpoints: no
  `CodegenIntegration` and no `static-cluster-config` include in any of them,
  so no SDK patch and no new `static-cluster-config/` file (the directory is
  still only `BooleanState.h` and `Descriptor.h`). Color Control's server init
  is routed by the regenerated dispatch in
  `cluster-init-callback.cpp` to a weak
  `emberAfColorControlClusterInitCallback(EndpointId)`; the port overrides it
  in batch 2's task 2. As with batch 1 this is a data model and image-link
  change only: the five enabled clusters carry no attribute data until the
  registry rows that create them arrive.
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
| `GENERATED_CLUSTER_COUNT` | 28 (23 before catalogue batch 2 enabled the five above) |
| `ATTRIBUTE_LARGEST` | `(66)` |
| `ATTRIBUTE_MAX_SIZE` | `(185)` (112 before catalogue batch 2 enabled the five above; 46 before task 3) |
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

`sdk-patches/README.md` is the mechanism, the patches, and how a reader tells
whether a tree is patched. The short version: the extension is copied out of the
Conan cache by `fw/sdk-prepare.sh`, which applies
`sdk-patches/matter_sdk/*.patch` (against `third_party/matter_sdk`) and then
`sdk-patches/extension/*.patch` (against the extension root itself) after
checking each against its own `.sha256`, and stamps the copy with
`HEARTH_SDK_PATCH_REV`; `toolchain.env` returns 1 naming the prepare script if
that stamp is missing or at the wrong revision. It is a revision check and not
a presence check for the same reason the matter_sdk patch needs one: that
patch defaults to stock behaviour when its macro is unset, so a stale cut of
it looks exactly like a current one; the stamp's revision covers the whole
tree, both patch directories, so an addition to either bumps it.

The matter_sdk patch caps `ElectricalEnergyMeasurement`'s `gMeasurements` table
at `CHIP_CONFIG_ELECTRICAL_ENERGY_MEASUREMENT_MAX_INSTANCES` (8, in
`src/CHIPProjectConfig.h`) instead of the whole dynamic endpoint space. The
cluster is not in this round's build, so the reclaim is not measured here.

The extension patch adds the `BooleanState` static cluster configuration
template that catalogue batch 1's task 2 needed (`sdk-patches/README.md`
explains why the extension tree and not `matter_sdk`): `matter_boolean_state`
is a code-driven cluster whose `CodegenIntegration.cpp` unconditionally
includes `app/static-cluster-config/BooleanState.h`, and the extension's own
`app-templates.json` only ever generates that header for `Descriptor`. The
patch adds a matching template for `BooleanState`, so `zap-regen.sh`'s
committed `data_model/zap-generated/app/static-cluster-config/BooleanState.h`
and slc's own `autogen/` copy are both produced the same way `Descriptor.h`
already is, and no include path anywhere else changes.

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

#### A log line from a fault handler does not wait for the mutex

`hearth_log_write()` (`port/hearth_port_sl.c`) used to take the console mutex
with `portMAX_DELAY` from whatever context called it. The log route above turned
that into a hazard, because three of the callers
`src/sdk/SoftwareFaultReports.cpp` adds are not task context:

- `debugHardfault()`, reached from `HardFault_Handler`, `BusFault_Handler`,
  `UsageFault_Handler`, `mpu_fault_handler`, `SecureFault_Handler`,
  `DebugMon_Handler` and `WDOG0_IRQHandler`;
- `vApplicationStackOverflowHook()`, called from `vTaskSwitchContext()` inside
  the PendSV handler, i.e. during a context switch;
- `RAILCb_AssertFailed()`, called from the radio interrupt.

A FreeRTOS mutex must not be touched from an interrupt with any timeout, and if
the faulting task happened to hold the lock the handler waited forever, which is
the silent fault the strong hooks were added to end. `log_may_block()` now
answers the question once for every caller: no lock in handler mode
(`__get_IPSR() != 0`), none before the scheduler runs or while it is suspended,
and a direct lock-free write at the peripheral instead. The cost is that a fault
report can interleave with a line another task is mid-way through.

Measured on the bench, 2026-09-18, with a scratch probe (never committed) that
took `s_log_lock` and then wrote to `0xFFFFFFF0`. Before the guard, the console
stopped dead at the probe's own line and the whole report was lost:

```
I boot: +MTREADY sent, free heap 98968 B
I boot: fault probe: taking the log lock, then writing 0xFFFFFFF0
                                     (nothing further, AT link 10 bytes)
```

After it, the report is complete:

```
I boot: fault probe: taking the log lock, then writing 0xFFFFFFF0
E chip: [-] HardFault:  0x48415244
E chip: [-] SCB->CFSR   0x00008200
E chip: [-] SCB->HFSR   0x40000000
E chip: [-] SCB->MMFAR  0xfffffff0
E chip: [-] SCB->BFAR   0xfffffff0
E chip: [-] SP          0x20025880
... R0 through PSR ...
```

`0x48415244` is `debugHardfault`'s `'HARD'` tag; CFSR `0x00008200` is
PRECISERR with BFARVALID, and BFAR is the address the probe wrote.

**A probe that faulted WITHOUT holding the lock printed the report either way**,
which is why the contended case is the one that was measured: an uncontended
`xSemaphoreTake` from an interrupt happens to work on this port, so a check
against the easy case would have passed with the bug still in.

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
08052bfc T BusFault_Handler
08052ab0 T debugHardfault
08052bd4 T HardFault_Handler
08052d00 T RAILCb_AssertFailed
08052c10 T UsageFault_Handler
08052cd0 T vApplicationGetIdleTaskMemory
0806e582 W vApplicationIdleHook
08052c4c T vApplicationMallocFailedHook
08052c80 T vApplicationStackOverflowHook
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

### Port sections: what has transferred from the nRF arm

The upward port is a section-by-section transfer of the nRF54L15 port's two
port files, so the two arms stay diffable. Each section carries the nRF line
range it came from as a banner comment in the Silabs file; this table is the
index, and a round that changes one arm finds the other through it.

| Section | nRF source and lines | Silabs file | Landed |
|---|---|---|---|
| commissioning state, network, Thread | `platform/nrf54l15/port/mt_matter_zephyr.cpp` 388-602 | `port/mt_matter_sl.cpp` | round 2 task 4 |
| the live endpoint table | `mt_matter_zephyr.cpp` 604-663 | `port/mt_matter_sl.cpp` | round 2 task 5 |
| the attribute bridge and the `+MTATTR` URC | `mt_matter_zephyr.cpp` 665-1505 | `port/mt_matter_sl.cpp` | round 2 task 6 |
| shared cluster building blocks | `mt_devtypes_zephyr.cpp` 276-320 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| on/off light (0x0100) | `mt_devtypes_zephyr.cpp` 321-348 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| temperature sensor (0x0302) | `mt_devtypes_zephyr.cpp` 388-416 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| dimmable light (0x0101) | `mt_devtypes_zephyr.cpp` 349-387 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| boolean-state sensors (0x0015, 0x0044, 0x0041, 0x0043) | `mt_devtypes_zephyr.cpp` 417-479 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| occupancy sensor (0x0107) | `mt_devtypes_zephyr.cpp` 480-538 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| humidity sensor (0x0307) | `mt_devtypes_zephyr.cpp` 539-563 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| pressure sensor (0x0305) | `mt_devtypes_zephyr.cpp` 564-588 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| light (illuminance) sensor (0x0106) | `mt_devtypes_zephyr.cpp` 589-620 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| flow sensor (0x0306) | `mt_devtypes_zephyr.cpp` 621-644 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| on/off plug-in unit (0x010A) | `mt_devtypes_zephyr.cpp` 645-670 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| dimmable plug-in unit (0x010B) | `mt_devtypes_zephyr.cpp` 700-721 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 |
| color temperature light (0x010C) and extended color light (0x010D) | `mt_devtypes_zephyr.cpp` 745-946 | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| thermostat (0x0301) | `mt_devtypes_zephyr.cpp` 947-1040 | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| fan (0x002B) | `mt_devtypes_zephyr.cpp` 1041-1100 | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| window covering (0x0202) | `mt_devtypes_zephyr.cpp` 1125-1199 | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| air quality sensor (0x002C) | `mt_devtypes_zephyr.cpp` 1200-1247 | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| the parenting policy | `mt_devtypes_zephyr.cpp` 3325-3428 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the registry | `mt_devtypes_zephyr.cpp` 4402-4641 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the external attribute store | `mt_devtypes_zephyr.cpp` 4642-4658 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the endpoint block arena and its sizing | `mt_devtypes_zephyr.cpp` 4659-5978 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the seed table and `seed_slots()` | `mt_devtypes_zephyr.cpp` 5979-7005 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the ember cluster init hook | `mt_devtypes_zephyr.cpp` 7006-7051 | `port/mt_devtypes_sl.cpp` | catalogue batch 1 (fix round 2, B525) |
| the ColorControl cluster init hook | the second override beside the first, this port's own | `port/mt_devtypes_sl.cpp` | catalogue batch 2 |
| `mt_dyn_attr_slot()` | `mt_devtypes_zephyr.cpp` 7052-7069 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the `mt_devtypes.h` quartet | `mt_devtypes_zephyr.cpp` 7189-8413 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the ember external-attribute hooks | `mt_devtypes_zephyr.cpp` 8415-8451 | `port/mt_devtypes_sl.cpp` | round 2 task 5 |
| the boot rebuild | `platform/nrf54l15/src/main.cpp` 40-108 | `src/main.cpp` | round 2 task 5 |
| the store handle and the arena | `platform/nrf54l15/port/mt_dyn_store.h` | `port/mt_dyn_store.h` | round 2 task 5 |

The nRF line ranges are against that file as it stands in the firmware
repository on `dev/fota-firmware`; they are a reading aid, not a promise that
the file has not moved since.

**What is NOT in that table, and is not waiting on a catalogue batch either:
the platform event bridge** (graph B502). `+MTEVT` is core's surface
(`mt_at_event()`, `core/mt/mt_at.c:1620`, with the subscription mask
`AT_MT_SPEC.md` 3.11 specifies), and the only call site in this repository is
the ESP32-C6's `platform/esp32c6/main/main.cpp`. **On this arm the consequence
is measured** (no URC of any kind during either commissioning, with the mask
wide open); **that the nRF arm is in the same position is inferred** from that
same call-site grep, not from an nRF board on a bench. So there is no section
to transfer: the row a future table gains is a CHIP platform event handler
registered with `PlatformMgr().AddEventHandler()` mapping `DeviceEventType` to
the bit numbers the spec allocates, written once for both Thread arms. See
"Commissioning" and "What round 2 leaves open".

Everything in `core/include/mt_matter.h` that has no row above is still
answered by `port/mt_matter_stub.c`. `test/host/check_decls.py` (run by
`make -C test/host silabs-stubs`) proves the pair between them defines every
one of the header's 61 declarations exactly once, so a function that moves and
is not deleted from the stub is a gate failure rather than a link-time
surprise.

What the transfer rewrites rather than copies, for this first section:

- Zephyr's `LOG_ERR()` becomes `HEARTH_LOGE("matter", ...)` with the same
  format strings (nRF 455, 460). The core log macro carries a printf format
  attribute, so `"%" CHIP_ERROR_FORMAT` is checked at compile time here in a
  way the Zephyr macro did not check it.
- The nRF's `CONFIG_OPENTHREAD_FTD` note (nRF 503-505) becomes a reference to
  `CHIP_DEVICE_CONFIG_THREAD_FTD`, which is 1 in `src/CHIPProjectConfig.h` and
  is what `hearth_matter_init.cpp`'s `InitOpenThread()` already relies on when
  it sets the device type to Router. Same conclusion, this tree's spelling:
  `otThreadIsRouterEligible()` is always reachable and needs no `#if`.
- `chip::DeviceLayer::StackLock` stays exactly as it was. It is CHIP's, not
  Zephyr's, and on this platform it is load-bearing:
  `SL_MATTER_STACK_LOCK_TRACKING_MODE` is `SL_MATTER_STACK_LOCK_TRACKING_FATAL`,
  so an unlocked CHIP call from the AT parser task kills the device instead of
  racing quietly. That setting is **not** a file in this repository. It is the
  extension's own `slc/config/sl_matter_config.h`
  (`$MATTER_EXT_ROOT/slc/config/sl_matter_config.h:12`, where FATAL is also the
  documented default), which slc copies into the generated project as
  `config/sl_matter_config.h`; `~/silabs/work/hearth-matter-t4/config/
  sl_matter_config.h:12` is the copy this image was built against. The only
  header `platform/silabs/config/` holds is
  `sl_openthread_features_config.h`. Whether the nRF arm's lock discipline is
  enforced the same way was not checked and nothing here claims it.
- The OpenThread reads keep the OT API and take the instance from
  `ThreadStackMgrImpl().OTInstance()` under
  `ThreadStackMgr().LockThreadStack()` / `UnlockThreadStack()`, which is the
  same pair `hearth_matter_init.cpp` hands to the Inet layer as its native
  params.
- `<setup_payload/OnboardingCodesUtil.h>` is the header's path in this tree,
  as it is in the nRF's. There is no `<app/server/OnboardingCodesUtil.h>`.

One lock-ordering fact, because it is the kind of thing that is cheap to state
and expensive to rediscover: `mt_matter_net_info()` holds the CHIP stack lock
and calls `ConnectivityMgr().IsThreadEnabled()`, which takes the OpenThread
lock itself
(`GenericThreadStackManagerImpl_OpenThread.hpp:244-254`). That is CHIP lock
then OT lock, the same order the CHIP event loop uses. `mt_matter_thread_info()`
takes the OT lock **alone** and never reaches for the CHIP lock underneath it,
so the port has no path that acquires them the other way round. Keep it that
way.

#### What task 6's transfer rewrote, and the two things that came with it

The attribute bridge itself transferred almost unchanged: `attr_type_info()`,
`attr_locate()`, `attr_null_sentinel()`, `mt_matter_attr_read()`,
`mt_matter_attr_write()` and `MatterPostAttributeChangeCallback()` are the nRF's
functions with `LOG_ERR` spelled `HEARTH_LOGE` and `Status` written out in full.
The as-built rules hold as stated: one external store, the ember path rather
than `mt_dyn_attr_slot()`, both notify modes echo `+MTATTR`, a same-value write
answers `OK` and echoes nothing, a transition to null never emits a URC, and
`MT_ATTR_ERR_READONLY` is unreachable from this bridge (there is no
Instance-served cluster in the image to make it reachable). All of those are
bench transcripts under "Measured", "Round 2 task 6".

Four things differ from the nRF arm, and each is stated in the source too.

**The Instance-served carve-out mechanism is present, and its rows are not the
nRF's.** Not one of the clusters the nRF's table names is compiled into this
image, which declares four (OnOff, Identify, Descriptor, TemperatureMeasurement)
and serves none of them from a per-endpoint C++ object; the section lists every
one of those rows with the batch that brings it back, and the rule for which
rows exist at all. What this image needs the mechanism for is the FIXED
endpoint: ruling F500's four Basic Information integer attributes, read from the
device instance info provider and refused on write. See "Fixed endpoints are not
readable through the ember path" below. The first landing of this section left
the mechanism out because the table would have been empty; the fix round put it
back with rows of its own.

**The nRF's BooleanState bridge inside the change callback goes with it**, for
the same reason: no endpoint in this image carries BooleanState, so the
`FindClusterOnEndpoint()` call would answer `nullptr` every time.

**`MatterPostAttributeChangeCallback`'s declaring header is included**, which
the nRF arm does not do. A strong override whose signature has drifted from the
weak default does not fail to link; it silently stops being called. Including
`app/util/generic-callbacks.h` makes the compiler prove the match.

**The read's failure arm is reachable here and the nRF calls it defensive.**
See "Fixed endpoints are not readable through the ember path" below; the arm
logs rather than failing silently.

**`MT_ATTR_ERR_READONLY` is reachable here and unreachable on the nRF arm at
the same point in its history**, for the same reason: the carve-out has rows.
It is still not reached by an `IsWritable()` check, which would wrongly refuse
`MeasuredValue` and `IdentifyType`; the whole argument is carried in the source
with this tree's own citation (`attribute-table.h:74-85`).

#### The 64-bit value the libc could not print

`AT+MTATTR` is the first AT command on this image to print a 64-bit integer,
and it did not print one. `AT+MTATTR=1,6,0` answered

```
+MTATTR:1,6,0,lu
```

The ARM GNU 12.2 toolchain's **newlib-nano is compiled without
`_WANT_IO_LONG_LONG`**, so its printf family does not know the `ll` length
modifier at all: it consumes `%l`, meets a second `l`, treats that as an unknown
conversion and copies the remainder out literally. `core/mt/mt_at.c` formats the
value with `%llu` / `%lld` through the port's `hearth_link_write_line()`, a
plain `vsnprintf()`, so the two characters `lu` went on the wire where the
number belonged. Nothing failed, nothing warned. `AT_MT_SPEC.md` 3.8 makes the
full width binding: "`<val>` is full-width 64-bit decimal ... so
`18446744073709551615` round-trips on a u64 attribute".

Fixed at the formatter rather than at the call site, because the gap was never
this bridge's alone: CHIP logs node and fabric ids with `PRIx64` and OpenThread
prints 64-bit extended addresses, so every one of those has been losing its
value on this image since round 1. `hearth.slcp` now carries the SDK's
**`printf`** component (third party, MIT, the mpaland implementation), which
replaces `printf`, `sprintf`, `snprintf`, `vsnprintf`, `vprintf` and `puts`
image-wide and supports `ll`. Cost: **3,272 B of `text`**, no `.bss` or `.data`
change, no allocator change, no `memcpy` change.

**`nano_c_libs: disabled` was tried first and rejected on evidence**, and it is
recorded because it is the obvious thing to reach for. slc's own toolchain
option drops `--specs=nano.specs` and links the full newlib, whose printf is
correct. The image builds warning-free and then **hard faults during
`Server::Init`**, before `+MTREADY`:

```
E chip: [-] HardFault:  0x48415244
E chip: [-] SCB->CFSR   0x01000000      (UsageFault, UNALIGNED)
E chip: [-] PC          0x0806684c      memcpy + 0x78
E chip: [-] LR          0x08090f45      nvm3_halFlashReadWords + 0x2c
E chip: [-] R0 0x20027204  R1 0x08175762  R2 0x00000004
```

`nvm3_hal_flash.c:159-167` calls `memcpy()` precisely and only when the source
or destination is **not** word-aligned, which is what R1 shows, and full
newlib's `memcpy` makes unaligned word accesses that this part traps.
newlib-nano's byte-wise `memcpy` was quietly surviving that path. It also cost
68,972 B of `text`. Do not re-try it without solving the NVM3 `memcpy` fault
first.

##### Where the tiny printf disagrees with newlib-nano

Read out of the component's own source (`$SISDK_ROOT/printf/printf.c`) rather
than assumed. One of these is a fault and the rest are cosmetic.

| Case | Tiny printf | newlib-nano |
|---|---|---|
| `%s` with a NULL argument | **dereferences it and faults** (`case 's'` at `printf.c:801-804` hands the pointer straight to `_strnlen_s()` at `:177-182`, which reads `*s` with no NULL check) | printed `(null)` |
| `%n`, `%a` | unimplemented: the default arm prints the conversion character itself (`printf.c:851-854`) | `%n` implemented, `%a` implemented |
| `%p` | uppercase hex, zero-padded to the pointer width, **no `0x` prefix** (`printf.c:828-830`) | lowercase with `0x` |
| `%f` above 1e9 | switches to exponential (`PRINTF_MAX_FLOAT`, `printf.c:86-88`, the switch at `:366`) | prints the long decimal form |

**The NULL `%s` is the one that matters**, and it changes the severity of a
class of defect rather than the look of a line: a NULL string in any log line in
this image, the SDK's, CHIP's or OpenThread's, is now a hard fault where it used
to be an ugly line.

**Nothing in `core/` or in this port passes one.** Every `%s` argument is a
string literal, a fixed-size buffer, or a value already NULL-guarded at the call
site, and the one place it could have happened is guarded deliberately rather
than by luck: `core/mt/mt_at.c:1715-1719` tests `mt_thread_role_name()`'s NULL
return (this port's own function returns `nullptr` for a role outside the known
set) and substitutes a number; `:1646-1651` and `:2180-2186` branch on their
optional argument instead of formatting it.

**The SDK's, CHIP's and OpenThread's call sites were not audited**, and that is
the open half. The one exception is the one SDK file this tree compiles,
`src/sdk/SoftwareFaultReports.cpp`, which was audited: its three `%s` arguments
(`filename` at `:112`, `pcTaskName` at `:241`, `errorMessage` at `:323`) are
non-NULL by construction, from `__FILE__` at the assert site, from the task's
own TCB and from a table lookup whose default is the literal `"Unknown"`; the
rest of the SDK, CHIP and OpenThread remain unaudited. A guard belongs to the
qualification round, and there are two ways to give it: patch the component
under `sdk-patches/` so `case 's'` prints `(null)` for a NULL pointer, or audit
the call sites. Patching is the cheaper
and the more honest of the two, because it fixes every call site including the
ones a future SDK version adds.

#### Fixed endpoints are not readable through the ember path

`AT+MTATTR=0,0x0028,0x0002`, the root node's VendorID, answers a bare `ERROR`
on this image, and it is the one Phase 1 `MTATTR` row that stays red. The cause
is a tree difference, not a bug in the bridge.

This SDK's CHIP serves the fixed endpoints' framework clusters through
registered cluster objects: `autogen/zap-generated/CodeDrivenInitShutdown.cpp`
constructs `BasicInformation`, `Descriptor`, `AccessControl` and the rest at
endpoint 0, and ZAP therefore declares their attributes `EXTERNAL_STORAGE` so
ember holds no bytes for them (`autogen/zap-generated/endpoint_config.h:76` is
the VendorID row; 156 attributes in the generated config carry that mask). A
controller's read never notices, because the data model provider asks the
cluster registry before it reaches ember. `emberAfReadAttribute()` does not: it
sees `EXTERNAL_STORAGE` and calls `emberAfExternalAttributeReadCallback()`,
which is this port's and answers only for the dynamic endpoints' arena.

Both arms' `hearth.zap` declare VendorID `External`, and the nRF arm passes the
row, so the divergence is in what the two CHIP versions generate from that
declaration rather than in either port.

**Controller ruling F500 carved out the four that the wire contract asks for.**
The Instance-served carve-out mechanism the nRF arm uses for its dynamic
endpoints (the table, `instance_attr_served()`, a read dispatch and a write
disposition) is present in `port/mt_matter_sl.cpp`, and its rows here are Basic
Information's four integer attributes on the root node:

| Attribute | Id | Source | Value on this image |
|---|---|---|---|
| VendorID | `0x0002` | `GetDeviceInstanceInfoProvider()->GetVendorId()` | 65521 (`0xFFF1`) |
| ProductID | `0x0004` | `...->GetProductId()` | 32784 (`0x8010`) |
| HardwareVersion | `0x0007` | `...->GetHardwareVersion()` | 1 |
| SoftwareVersion | `0x0009` | `ConfigurationMgr().GetSoftwareVersion()` | 1 |

The provider is the same source the registered `BasicInformationCluster` object
reads from, so an `AT+MTATTR` read answers what a subscribed controller sees
rather than a second copy of it that can drift. SoftwareVersion comes from
`ConfigurationMgr()` because that is where this tree's interface puts it:
`DeviceInstanceInfoProvider.h` declares `GetVendorId`, `GetProductId` and
`GetHardwareVersion` but no `GetSoftwareVersion`, and
`ConfigurationManager.h:105` declares it instead.

**They are read-only.** A write answers `MT_ATTR_ERR_READONLY`, `+MTERR:11`,
which `AT_MT_SPEC.md` 3.8 defines for "an attribute that exists but is served by
a cluster Instance and cannot be written over AT". **That code became reachable
on this image for the first time with this change**; it was unreachable when
the bridge first landed, and the comment in `mt_matter_attr_write()` records the
transition rather than restating the old claim.

**The known limit, stated plainly: fixed-endpoint code-driven clusters are
readable through `AT+MTATTR` only where the carve-out serves them. The general
provider read path is a later round's.** Everything else on endpoint 0 answers
as before: Basic Information's strings and its `CapabilityMinima` struct answer
`+MTERR:5` on type, before ember is reached, and every integer attribute of
Access Control, General Commissioning, General Diagnostics and the rest answers
`MT_ATTR_ERR_FAILED`, a bare `ERROR`, with the log line above naming the
endpoint, cluster, attribute and ember status. `MT_ATTR_ERR_FAILED` is the
honest code for those: the attribute exists and is an integer, and this path
could not read it; `MT_ATTR_ERR_ATTRIBUTE` would claim it does not exist.

The four rows are deliberately four rows and not a mechanism for reaching the
provider. A general path needs an `AttributeValueEncoder` over a TLV writer and
a decode back, it has no nRF counterpart to transfer, and it would change the
answer for every fixed-endpoint attribute rather than the ones the contract
names.

#### The unported families' stubs know the live endpoints (step 1b)

`port/mt_matter_stub.c` gained `stub_endpoint_live()`, one loop over the live
endpoint table, and every stub that takes an endpoint id and whose
`core/include/mt_matter.h` entry documents both an endpoint error and a
cluster error now answers through it. Before this they all answered "no such
endpoint" unconditionally, which was right only while no endpoint existed at
all. It stopped being right the moment task 5 stood up the rig's light:
eighteen Phase 1 rows exist precisely to prove a host can tell "that endpoint is
not there" from "that endpoint is there and does not do this".

Which code means "no such cluster" is read from each family's own header entry
rather than assumed, and the three answers are not the same:

| Stub | Live-endpoint answer | On the wire |
|---|---|---|
| `mt_matter_switch_click`, `mt_matter_temp_levels_set`, `mt_matter_lock_state_set`, `mt_matter_valve_state_set`, `mt_matter_modes_set`, `mt_matter_modebase_set`, `mt_matter_opstate_set`, `mt_matter_alarm_set`, `mt_matter_chime_sounds_set`, `mt_matter_chime_set`, `mt_matter_meas_set`, `mt_matter_demcap_set`, `mt_matter_evse_set` | `MT_ATTR_ERR_CLUSTER` | `+MTERR:3` |
| `mt_matter_rows_apply`, `mt_matter_rows_get`, `mt_matter_rows_total`, `mt_matter_evse_targets_apply`, `mt_matter_evse_targets_get`, `mt_matter_evse_targets_total` | `MT_ROW_ERR_NO_PAYLOAD` | `+MTERR:4` |
| `mt_matter_meter_set_identity` | `MT_ATTR_ERR_ATTRIBUTE` | `+MTERR:4` |

The row family has no `MT_ROW_ERR_CLUSTER` at all: `mt_rows.h`'s codec owns that
space and `mt_matter.h:1152-1163` says so. `mt_matter_meter_set_identity()`'s
header entry says "deliberately not `MT_ATTR_ERR_CLUSTER`" and gives the reason,
which is why it is the one row on its own. Stubs whose family takes no endpoint
id are unchanged, and so is every delegate allocator: a pointer return has no
error code to divide.

Twenty stubs changed, no data model is touched by any of them, and each is
replaced whole by the batch that ports its family.

#### What task 5's transfer rewrote, and the registry policy it landed

The device-type side is the round's largest transfer and it changed three
things about the nRF's shape. All three are stated in the source too; this is
the index.

**The two Zephyr `K_HEAP_DEFINE` arenas became one static bump arena**
(`port/mt_dyn_store.h`, `hearth_arena`). There is no second heap on this
platform to model, and drawing endpoint blocks from the one that exists would
be drawing from the pool that holds FreeRTOS's objects, CHIP's allocations,
mbedTLS and OpenThread ("The heap changed shape" above), so an oversized
composition could starve the stack instead of failing at the endpoint that
does not fit. A bump arena keeps all three properties the nRF's dedicated heap
was chosen for (contained failure, a failure log that can name what was left,
an auditable line item) and gives up nothing, because the nRF's heap is
allocate-only as well: nothing frees, and a reboot resets it.

**The cost model is therefore rounding and nothing else**, and every sizing
assertion was recast on it. A block costs its payload rounded up to 8; the
arena's usable bytes are its gross bytes. What went with the allocator: the
nRF's `kHeapCostOf` (`roundup(payload + 4, 8)`, the `sys_heap` chunk header),
its `kObjCostOf` (`roundup(payload, 8) + 8`), the 80-byte `kHeapOverheadBytes`
derivation and the two bucket-band `BUILD_ASSERT`s that keep that 80 honest.
Those are arithmetic about an allocator this image does not link.
`BUILD_ASSERT` became `static_assert`, and
`sys_heap_runtime_stats_get()` became `cap - used`.

**The registry policy.** All 52 catalogue rows are present and every row keeps
its identity: its device type id, its `max_variant`, and the parenting rule
`parent_policy_ok()` keys on. That is what `mt_devtype_is_known()`,
`mt_devtype_variant_ok()` and `mt_devtype_parent_ok()` answer from, and
`core/mt/mt_at.c` calls all three on the `AT+MTEP=` line itself, so **the AT
surface answers for the whole catalogue exactly as the nRF's does**: a cook
surface with no cooktop parent is `+MTERR:1` here today, a water heater's
variant 1 is accepted, and a device type outside the catalogue is `+MTERR:6`.
What a row for an unported type does not carry is a cluster set, and
`mt_devtype_create()` refuses such a row at its first check, so a composition
naming one is staged and persisted like any other and then fails its rebuild
at that entry, keeping the endpoints before it as a live prefix. The rows are
kept rather than deleted deliberately: deleting them would make
`AT+MTEP=0x000A` answer "unknown device type", which is a different and wrong
statement about a product whose wire contract names all 52. Each unported row
carries a comment naming the nRF batch that will build it.

Three consequences of that policy are worth stating out loud:

- **Two device types are creatable this round**, the on/off light (0x0100) and
  the temperature sensor (0x0302). Every other row answers "not supported" at
  apply, on the console, with the endpoints before it live.
- **The compile-time proof that the parenting policy and the cluster sets
  agree** (`shape_domain_matches_policy()`, nRF 4543-4641) is kept and recast:
  it skips rows with no cluster set, because such a row has nothing for the
  policy to leave unserved, and checks the creatable rows in full over the
  whole 52-row parent universe. The day a batch gives the cabinet or the cook
  surface an `ep_type` without a shape map, it fails this build exactly as it
  would fail the nRF's.
- **The floor under the arena sizing is STRONGER here than on the nRF**, and
  deliberately so. The nRF demands room for eight of its widest uncapped
  device type, because sizing for sixteen of its heaviest is a trade it
  declined. This catalogue's widest block was the on/off light's 192 B at
  round 2, when the arena held `kServiceableEndpoints` of it in 3,072 B and
  the promise was "every composition this build accepts, it can build".
  Catalogue batch 1 was the first batch to add a wider type (the dimmable
  light and plug, 336 B): rather than dropping to the nRF's floor of eight,
  it kept the strong promise by raising `HEARTH_EP_ARENA_BYTES` to 5,376 B
  (the ruling of 2026-09-22). See "Catalogue batch 1" under "Measured" for
  the sizing table.

Four parts of the nRF's device-type side are **absent rather than reduced**,
each because every consumer it has belongs to an unported device type: the
per-endpoint delegate handout in `mt_devtype_create()` and the
cluster-object arena it draws on (`mt_matter_zephyr.cpp` 133-386 and
9137-9472); the type-conditional trailing stores (`kStoreWalk` and the four
host-fed store shapes); the DE407 quiet table; and the hand-called B388
cluster-init call site (`LevelControl`, `ColorControl` and `ModeSelect`
`ServerInit`, two of whose cluster servers are not even in this image). The
sources say what the batch that needs each one has to bring back.

`port/mt_devtypes_stub.c` is now an empty file, kept rather than deleted so
that `hearth.slcp`, `test/host/Makefile`'s `silabs-stubs` target and
`check_decls.py`'s pair list keep naming the same pair; `check_decls.py` proves
the four `mt_devtypes.h` declarations have exactly one definition across the
concatenation, which is the property that matters whether the split is 4/0 or
0/4.

One stub answer changed with this task, and it is a ruling rather than a
convenience: `mt_matter_evse_targets_erase_all()` returns 0. `cmd_mtfreset()`
calls it before `mt_matter_factory_reset()`, this image has no EVSE targets
store at all, and erasing nothing succeeded. Returning -1 would fail
`AT+MTFRESET` on a device whose state is already what the command asks for.
`AT+MTFRESET` is exercised on the bench below.

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

**Phase 2 needs a second carrier fact**, `--openocd-config`, because its steps
2.8, 2.13 and 2.14 reboot the co-processor over SWD:

```bash
python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 2 \
        --openocd-config platform/silabs/mg24-swd.cfg \
        --include-slow --include-manual --baseline platform/silabs/core-phase2.json
```

The harness's built-in argv resets an **RP2350** through
`interface/cmsis-dap.cfg`: on the C6 rig the probe is on the Challenger's
bridge and the bridge sketch's `setup()` pulses the C6's reset line. Here the
Debug Probe is on the MGM240P's own SWD, so `platform/silabs/mg24-swd.cfg`
names the target (`target/silabs/xg24.cfg`, `reset_config none`, so `reset run`
goes through the Cortex-M33's `SYSRESETREQ`) and, because this bench carries
**two** CMSIS-DAP probes that both answer SWD with a Cortex-M33, names the
adapter by serial as well. Without the serial `interface/cmsis-dap.cfg` picks
whichever enumerates first and resets the wrong board, silently. Only the two
`-f` arguments change; the reset command is the harness's for every carrier.

`--baseline` with `--phase 2` requires `--include-slow` and `--include-manual`
by the harness's own door check, so a Phase 2 record here always includes the
200 s window-expiry row and the operator-driven cold boot.

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

**Hearth is commissioned on this carrier, twice, 2026-09-18** (round 2 task 7,
the round's bench acceptance). The procedure below is the Hearth image's; the
round 1 record of the stock Silicon Labs sample is kept after it, because it is
what proved the radio and the border router before Hearth had a stack.

The Thread operational dataset is a credential, and `ot-ctl` cannot be used to
read it here. The reason is the socket, not the binary:
`/run/openthread-wpan0.sock` is `srwxr-xr-x root:root`, so any `ot-ctl` run as
this user answers `connect session failed: Permission denied` (measured, both
with the ot-br-posix build tree's own binary and with none on the shell's PATH
at all). otbr-agent's
D-Bus property serves the same value without sudo and without touching the
daemon, which stayed `active` throughout (`systemctl is-active otbr-agent`,
checked before and after every step):

```bash
DS="$(busctl --system get-property io.openthread.BorderRouter.wpan0 \
        /io/openthread/BorderRouter/wpan0 io.openthread.BorderRouter \
        ActiveDatasetTlvs --json=short \
      | python3 -c 'import json,sys; print("".join("%02x"%b for b in json.load(sys.stdin)["data"]))')"
CT=~/esp/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool
```

The dataset stays in a shell variable, is never printed and never written to a
file, and every hex run of 24 characters or more in chip-tool's output is
replaced before anything is quoted here. The Thread extended PAN id and network
name are redacted below as well: they are dataset fields, shorter than the
24-character rule catches, and nothing on this page needs them.

### The AT side, then the controller

1. `AT+MTEPCLEAR`, `AT+MTEP=256`, `AT+MTEP=770`, `AT+MTEPAPPLY` stages the
   acceptance composition (an on/off light and a temperature sensor). Apply
   reboots the module; the composition is rebuilt as dynamic endpoints before
   `+MTREADY`, so `AT+MTEP?` reads back what the device is serving.
2. `AT+MTSTATE?` reads `+MTSTATE:1,0` on a factory-fresh device: `Server::Init`
   opens the commissioning window on its own, so the window is already there.
   `AT+MTCOMMISSION` re-opens it if it has closed; it answers `OK` either way,
   and `AT+MTSTATE?` then reads `1,<fabrics>`.
3. chip-tool pairs over BLE and hands the device the dataset:

```bash
"$CT" pairing ble-thread 0x4901 "hex:$DS" 20202021 3840 \
      --storage-directory /tmp/ct-hearth-mg24
```

The discriminator `3840` and the passcode `20202021` are
`src/CHIPProjectConfig.h`'s `CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR`
(`0xF00`) and `..._USE_TEST_SETUP_PIN_CODE`. They are development credentials,
as is the VID `0xFFF1`: consumer hubs are expected to refuse them.

Both commissionings succeeded on the first attempt. **`fw/srp-aaaa-shim.sh` was
not needed**: chip-tool's operational discovery found the device on the same
host that runs the border router, and the CASE session came up without it. The
nRF arm needs that workaround; this one did not on 2026-09-18, and nothing was
changed on the border router to make that true.

| | First | Second |
|---|---|---|
| Node id | `0x4901` | `0x4902` |
| Before it | factory-fresh, `+MTSTATE:1,0`, `+MTFABRICS:0` | after `AT+MTFRESET`, composition restaged |
| chip-tool | exit 0 | exit 0 |
| Console | `Commissioning completed successfully` | `Commissioning completed successfully` |
| Fabric | index `0x1`, compressed id `0xFA98BD812AC6C240` | index `0x1`, compressed id `0xFA98BD812AC6C240` |
| `AT+MTSTATE?` after | `+MTSTATE:2,1` | `+MTSTATE:2,1` |
| `AT+MTFABRICS?` after | `+MTFABRICS:1` | `+MTFABRICS:1` |
| `AT+MTTHREAD?` after | `ROUTER`, attached | `REED`, attached |
| `AT+MTNET?` after | `+MTNET:THREAD,1,1,0` | `+MTNET:THREAD,1,1,0` |

The two Thread roles differ because they were read at different points in the
same promotion: a freshly attached device is a REED and is promoted to ROUTER
by the mesh a little later. Both are decoded from the running stack by
`mt_matter_thread_info()`, and a read 25 s after a reboot shows the settled
value:

```
AT+MTTHREAD?  ->  +MTTHREAD:UNASSIGNED,0,12,0x5CD1,0x[EXTPANID],0x00000000,"[NET]"
                                                        (immediately after boot)
AT+MTTHREAD?  ->  +MTTHREAD:ROUTER,1,12,0x5CD1,0x[EXTPANID],0x282CCFEA,"[NET]"
                                                        (25 s later, attached)
```

### The URC order observed: there is none, and why

**Closed 2026-09-21**, one round later, by the `+MTEVT` parity round: this port
emits the whole event surface now and the order is recorded under "Events
(`+MTEVT`)" below. The round 2 record is kept as written, because the
measurement below is what the fix was written against.

**No URC of any kind reached the AT link during either commissioning.** The AT
port was held open in listen mode for the whole of both runs, with no command
in flight, and the count was zero both times.

That is not a defect in this round's work and it is not a surprise once
located: **nothing in this port emits `+MTEVT` at all** (graph B502).
`mt_at_event()` is core's (`core/mt/mt_at.c:1620`), the subscription mask is
core's and works here, and the only call site in the whole repository is the
ESP32-C6's `platform/esp32c6/main/main.cpp`. **The nRF arm is therefore
expected to be silent too, but that half is inferred from that call-site grep
and from the committed `test/baselines/thread-lifecycle.json` having been
recorded on a Challenger port (the C6's Thread image). No nRF board was put on
the bench to confirm it.** The MG24 half is measured on this image rather than
inferred:

```
AT+MTEVT?             ->  +MTEVTMASK:0x0800003F / OK     the default mask
AT+MTEVT=0xFFFFFFFF   ->  OK                             subscribe to everything
AT+MTEVT?             ->  +MTEVTMASK:0xFFFFFFFF / OK
AT+MTCOMMISSION       ->  OK                             the window opens
AT+MTSTATE?           ->  +MTSTATE:1,1                   ... and it IS open
                          (no +MTEVT:0, and none for the 15 s after it)
```

So the surface exists and answers, and the platform half that would raise the
events is unported on both Thread arms. It needs a CHIP platform event handler
(`PlatformMgr().AddEventHandler()`) mapping `DeviceEventType` to the bit
numbers `AT_MT_SPEC.md` 3.11 allocates, which is a section with no nRF
counterpart to transfer. It is graph B502, and it is in "What round 2 leaves
open" with its owner.

The spec's acceptance item 4 asks for "the commissioning URCs arrive in order
after `+MTREADY`". This round cannot answer it, and says so rather than
quietly passing the item on the three checks it can answer.

### Control both ways

Both directions were exercised against both nodes. The AT port is held open in
listen mode while chip-tool runs and no AT command is in flight then, because
`ATLink` would otherwise absorb the URC into a command's response lines
(the harness's standing rule, T1 design section 8 N23).

```
$ chip-tool onoff toggle 0x4901 1
  Received Command Response Status for Endpoint=1 Cluster=0x0000_0006 Command=0x0000_0002 Status=0x0
  [AT URC t+0.81] +MTATTR:1,6,0,1
AT+MTATTR=1,6,0                  ->  +MTATTR:1,6,0,1 / OK
$ chip-tool onoff read on-off 0x4901 1
  OnOff: TRUE

AT+MTATTR=2,1026,0,2222          ->  +MTATTR:2,1026,0,2222 / OK
$ chip-tool temperaturemeasurement read measured-value 0x4901 2
  MeasuredValue: 2222
```

The same pair on `0x4902`, after the factory reset and the second
commissioning: the toggle's URC arrived at t+0.92 s, `AT+MTATTR=2,1026,0,1850`
read back from chip-tool as `MeasuredValue: 1850`. Both directions go through
the same arena slot (every attribute on a dynamic endpoint is
`EXTERNAL_STORAGE`), which is what makes this one test rather than two.

The controller sees the composition, not just the attributes:

```
$ chip-tool descriptor read parts-list 0x4901 0           PartsList: 2 entries
$ chip-tool descriptor read device-type-list 0x4901 1      DeviceType: 256 (On/Off Light)
$ chip-tool descriptor read device-type-list 0x4901 2      DeviceType: 770 (Temperature Sensor)
$ chip-tool basicinformation read vendor-id 0x4901 0       VendorID: 65521
$ chip-tool basicinformation read product-id 0x4901 0      ProductID: 32784
```

The last two are the other half of ruling F500: `AT+MTATTR=0,40,2` answers
`65521` and `AT+MTATTR=0,40,4` answers `32784` on the same image, so the AT
carve-out and the controller's own read of Basic Information agree.

### Reset and recommission

`AT+MTFRESET` erases the fabrics and the composition and reboots:

```
AT+MTFRESET   ->  OK          then +MTREADY
AT+MTEP?      ->  OK          no rows: the composition is gone
AT+MTSTATE?   ->  +MTSTATE:1,0
AT+MTFABRICS? ->  +MTFABRICS:0
```

The console's own boot line for that reboot says the same from the other side:
`no stored composition, starting unconfigured`, `composition rebuilt: 0
endpoint(s)`, `endpoint arena: 0 of 3072 B handed out`. The composition was
then restaged and the device commissioned again as `0x4902`, so two full
commissionings happened in one session on one image.

### Round 1: the stock Silicon Labs sample

Recorded 2026-09-17, stock Silabs `lighting-app` built for MGM240PA32VNA,
flashed as a `.gbl` through the bootloader above, against the same bench border
router, with the same D-Bus dataset read and the same redaction. It is kept
because it is what proved this module's radio and this bench's border router
before Hearth had a Matter stack at all.

| | |
|---|---|
| Node id | `0x51` |
| Fabric | compressed id `9F3A67057A34079E` |
| `onoff toggle 0x51 1` | endpoint 1, cluster `0x0000_0006`, command `0x0000_0002`, **Status 0x0 SUCCESS**, exit 0 |
| `basicinformation read vendor-name 0x51 0` | `Silabs` |
| `basicinformation read product-name 0x51 0` | `SL_Sample` |

The toggle and the two attribute reads went over the operational Thread
session, not BLE, so they prove the device joined the mesh.

## Events (`+MTEVT`)

Added 2026-09-21 by the `+MTEVT` parity round, task 1, which closes graph
**B502** on this port. The contract is `AT_MT_SPEC.md` 3.11 and the bit
numbers are `core/include/mt_at.h:47-136`; nothing in `core/` changed.

### The shared file, and the three sources behind it

`platform/chip/mt_chip_events.{h,cpp}`, listed in `hearth.slcp` under `source:`
and `include:`. It is **shared with the nRF arm** (task 2 lists it in that
port's `CMakeLists.txt`), which is the whole point: it names stock CHIP
symbols only and no SDK of either vendor, so one mapping serves both Thread
ports. The C6 keeps its own (`platform/esp32c6/main/main.cpp` `app_event_cb`),
because esp-matter's fork already posts the extra device events.

Two entry points, both called from `hearth_boot_task()` in `src/main.cpp`:

| Call | Where | Lock |
|---|---|---|
| `mt_chip_events_register()` | after `hearth_matter_init()` and the catalogue-endpoint disable, in the same `StackLock` block | the caller's stack lock |
| `mt_chip_events_after_ready()` | immediately after `mt_at_start()` | none: it reads `mt_matter_state()`, which takes and releases the lock itself |

Stock CHIP has no device event for the commissioning window, the commissioning
sessions or the fabric table, so the bits come from three places rather than
one:

| Bits | Source |
|---|---|
| 3, 5, 10 to 22, 24, 25, 28 | `PlatformMgr().AddEventHandler()`, a switch on `DeviceEventType` |
| 0, 1, 2, 4 | `CommissioningWindowManager::SetAppDelegate()`, an `AppDelegate` |
| 6, 7, 8, 9 | `Server::GetInstance().GetFabricTable().AddFabricDelegate()` |
| 26 | nothing posts it, on either SDK. The bit stays allocated |
| 27 | boot-only and single-transport: never raised on this image |

`initParams.appDelegate` is left unset by `port/hearth_matter_init.cpp`, so
`SetAppDelegate` replaces nothing. Every emission goes through
`mt_at_event()`, so the RAM mask and the pre-`+MTREADY` `s_at_up` guard apply
unchanged, and nothing in the file takes the CHIP stack lock: the handler and
both delegates already run on the CHIP task with it held, `mt_at_event()`
takes the link mutex only, and the bit-28 role read goes through
`mt_matter_thread_info()`, which takes the OpenThread lock alone on this port
(CHIP then OT, the established order).

### The boot replay

The window opens inside `Server::Init`, before `mt_at_start()`, so the
delegate's `+MTEVT:0` is dropped by the `s_at_up` guard.
`mt_chip_events_after_ready()` replays it once, which is what makes the boot
sequence **`+MTREADY` then exactly one `+MTEVT:0`** whenever a window is open,
instead of a race. It is the C6's block (`main.cpp:6865`) with the same
double test of the flag around `mt_matter_state()`.

### Bit 4 is decided off the CHIP event queue, and the C6's way does not work here

This is the one place the two SDKs differ in **shape** rather than in name,
and it cost a bench round to find. `+MTEVT:0` and `+MTEVT:4` are a strict
pair: exactly one 4 per delivered 0, at the moment the window is really gone.
CHIP stops advertising for PASE at three different moments (a commissioner
took a session and the window is only paused, the window genuinely ended, a
failed open cleaning up), so the 4 has to be gated on which one this is. The
C6 gates by re-querying `IsCommissioningWindowOpen()` inside the callback.

**On stock CHIP that answer is always "open".** esp-matter POSTS
`kCommissioningWindowClosed` as a device event, dispatched off the queue once
the window manager has finished; stock CHIP CALLS
`AppDelegate::OnCommissioningWindowClosed()` synchronously from
`StopAdvertisement()`, and the real-close path is `Cleanup()`, which is
`StopAdvertisement()` **then** `ResetState()`
(`CommissioningWindowManager.cpp:142-146`), so `mWindowStatus` is still
`kBasicWindowOpen` when the callback runs. Both moments look identical from
inside it. Measured, the first cut of this file: a full commissioning produced
1, 25, 28, 3 and **no 4 at all**, for 148 s after completion.

The fix is to do what esp-matter does and ask again off the queue.
`PlatformMgr().ScheduleWork()` posts a `kCallWorkFunct` onto the **same FIFO
queue the device events come from**, so the re-query is processed only once
the whole dispatch of the event that caused the close has finished. That is
what makes `ResetState()` have run by then, and it is also what puts the
`+MTEVT:4` after the `+MTEVT:3` of the same `kCommissioningComplete`: Hearth's
bit-3 emission is part of that same dispatch. The queue is the whole reason,
and handler order has nothing to do with it.

Do not reason from registration order here. That is the obvious-looking
argument and it is wrong twice over: the window manager registers its platform
handler inside `OnSessionEstablished()`
(`CommissioningWindowManager.cpp:241`), not in `Server::Init`, and
`_AddEventHandler` **prepends** (`GenericPlatformManagerImpl.ipp:199-203`), so
the later registration is dispatched first. The ordering this port depends on
survives either way, because it is the queue's.

If `ScheduleWork()` itself fails, which needs a full event queue, there is no
fallback to degrade to: every path into the callback runs before
`ResetState()`, so an immediate re-query would answer "still open" and emit
nothing. That window's `+MTEVT:4` is lost and the next window's `+MTEVT:0` is
suppressed with it until a reboot. The code says so on the console
(`ChipLogError`, `AppServer`) rather than dropping it silently. It has not
been reached on the bench.

### What was observed, 2026-09-21

Image `~/silabs/work/hearth-evt`, commit `679e3d8`, flashed with
`fw/flash.py`. Every capture below was re-taken on that image after the review
fix; the run on `ca62f42` before it gave the same event set in the same order,
which is what was expected, because the only code that fix changed is a
console line on a path no run has reached. The AT port was opened with DTR and RTS cleared before the open
(the `--bridge cpico` contract) and held open with no command in flight while
chip-tool ran. Timestamps are seconds from the port open, which is also the
module's reset. The dataset came from the D-Bus recipe above into a shell
variable and was never printed; every hex run of 24 characters or more in
anything quoted here is replaced.

**The boot sequence**, factory-fresh, 12 s capture, no commands sent:

```
t+  0.401  +MTREADY
t+  0.401  +MTEVT:0
```

Nothing else arrived in the remaining 11.6 s. On the boot of a **commissioned**
device, with no window open, the same capture gives `+MTREADY` and no
`+MTEVT:0` at all, which is the negative half of the same check.

**The mask**, then a full commissioning of node `0x4A01` (chip-tool exit 0,
first attempt, no `fw/srp-aaaa-shim.sh`):

```
t+  2.000  >>> AT+MTEVT?
t+  2.004  +MTEVTMASK:0x0800003F      the default: bits 0 to 5 and 27
t+  3.000  >>> AT+MTEVT=0x1A00003F    adds bits 25 and 28, before pairing
t+  3.006  OK
t+  4.000  >>> AT+MTCOMMISSION
t+  4.059  OK                         the window was already open: no second +MTEVT:0
t+  6.000  >>> AT+MTSTATE?
t+  6.012  +MTSTATE:1,0
t+ 14.735  +MTEVT:1                   PASE session established
t+ 19.848  +MTEVT:25
t+ 19.848  +MTEVT:28,UNASSIGNED
t+ 23.256  +MTEVT:25
t+ 23.306  +MTEVT:28,REED
t+ 23.356  +MTEVT:25                  role unchanged: no 28 beside this one
t+ 26.114  +MTEVT:3                   commissioning complete
t+ 26.114  +MTEVT:4                   the window really closed, after the 3
```

Three things in that trace are the design working rather than coincidence.
The **25 at t+23.356 has no 28 beside it**: `ThreadStateChange.RoleChanged`
was set and the decoded token had not changed, which is exactly the C6's bench
defect A and exactly what the role cache exists to suppress. The **4 follows
the 3** rather than racing it, for the queue reason above. And there was
**no second 4 and no second 0 for the next 94 s** (the capture ran to t+120,
far past the 10 s the round asked for), so the session pause that also fires
`OnCommissioningWindowClosed()` produced nothing.

`+MTEVT:2` is absent and that is correct, and the reason is narrower than the
header comment on `AppDelegate::OnCommissioningSessionStopped()` suggests. Its
only call site is `HandleFailedAttempt()`
(`CommissioningWindowManager.cpp:179-184`), and it is reached only when that
function's `AdvertiseAndListenForPASE()` failed or was never attempted because
the 20-attempt limit had been passed. A failed PASE attempt below the limit
does not produce it, and neither does a successful commissioning.

**The window reopened by hand**, which is what proves the delegate's own bit-0
path rather than only the boot replay, and that the 0/4 pair cycles:

```
t+120.000  >>> AT+MTSTATE?
t+120.018  +MTSTATE:2,1               operational, one fabric
t+123.079  +MTEVT:25
t+123.079  +MTEVT:28,ROUTER           the REED to ROUTER promotion, reported
t+125.000  >>> AT+MTCOMMISSION
t+125.035  +MTEVT:0                   the URC precedes the terminal response
t+125.085  OK
t+135.013  +MTSTATE:1,1
```

**`AT+MTFRESET`**, with the mask widened to `0xFFFFFFFF` first so the fabric
bits are visible:

```
t+150.000  >>> AT+MTEVT=0xFFFFFFFF
t+150.052  OK
t+152.000  >>> AT+MTFRESET
t+152.008  OK
t+152.158  +MTEVT:6                   FabricWillBeRemoved
t+152.158  +MTEVT:7                   OnFabricRemoved
t+153.261  +MTREADY                   the reboot
t+153.261  +MTEVT:0
```

So **the reset does remove the fabric through the fabric table**, and both
delegate callbacks reach the host before the reboot. One honest limit in that
trace: a commissioning window was open when the reset ran (`+MTSTATE:1,1` at
t+135.013) and **no `+MTEVT:4` arrived for it**. A `+MTEVT:4` across
`AT+MTFRESET` is a race between the deferred re-query and the reboot, and the
two Thread arms land on opposite sides of it: here the reboot wins and no 4 is
raised, while on the nRF54L15 the re-query wins, with the 4 arriving 1.3 s
after the command and `+MTREADY` 2.4 s later (`platform/nrf54l15/README.md`,
"The five facts a host or a harness row has to know", fact 5). A Phase 2 row
must neither require nor forbid a 4 there.

The deferred re-query has a second consequence worth stating beside it, and it
is benign: **a window re-opened between the close callback and the queued
check reports as one window, not two.** The check finds the window open, emits
no 4, and leaves `s_window_evt_sent` set, so the re-opened window raises no
second `+MTEVT:0` either. The host sees one 0 and, when that window really
ends, one 4. The pair count stays balanced, which is what the contract is
about; what is lost is only the knowledge that the window blinked.

### The cost

| | Round 2 task 7 | This round | Delta |
|---|---|---|---|
| `.text` | 844 504 B | 845 480 B | **+976 B** |
| `.data` | 3 368 B | 3 376 B | +8 B |
| `.bss` | 117 720 B | 117 720 B | 0 |
| `.memory_manager_heap` | 135 992 B | 135 984 B | -8 B |
| Free heap at `+MTREADY`, uncommissioned | 95 336 B | **95 304 B** | -32 B |

Flash cost **984 B** (`text` plus `data`). The `.bss` row is 0 because the
file's four statics (11 bytes) fit in existing alignment slack; the `.data`
row is the `AppDelegate` subclass's vtable pointer, constant-initialised, and
the heap region gives up the same 8 bytes because it is what is left of RAM.
`arm-none-eabi-size -A`, `~/silabs/work/hearth-matter-t7` against
`~/silabs/work/hearth-evt` at `679e3d8`, both read 2026-09-21. 48 B of the
`.text` figure is the review fix's console line alone (845 432 at
`ca62f42`). The heap figure is the boot task's own console line on the
uncommissioned boot quoted above
(`I boot: +MTREADY sent, free heap 95304 B`), which is the same measurement
point and the same state as task 7's 95 336 B row under "Measured".

## Harness Phase 2

Phase 2 is the Matter lifecycle chain: factory reset, commissioning, attribute
round trips in both directions, a second fabric and its removal, a warm reboot
over SWD, a cold boot, an unattended window expiring, the two resets, and the
rig restored to the bench's standard state. Round 2 recorded it as **not run**
(graph **F503**): the gate aborted on the bench's root-owned OpenThread socket,
zero rows executed, and the `+MTEVT` assertions in eight of its steps had
nothing to assert against (graph **B502**). The `+MTEVT` parity round closed
both halves, the events in task 1 and the gate's D-Bus route in task 3, and
this is the run they were for.

**Re-run on the catalogue batch 1 image on 2026-09-22 at the same 98 / 2 / 1
and the same row set**, the result file differing only in its header's commit
and timestamp; see "Catalogue batch 1" under "Measured". **Re-run on the
catalogue batch 2 image on 2026-09-24 at the same 98 / 2 / 1 and the same row
set** as well; see "Catalogue batch 2" under "Measured". The record below is
the first run, and it is where the two failing rows are explained.

### 2026-09-21: 98 passed, 2 failed, 1 not applicable

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 2 \
      --openocd-config platform/silabs/mg24-swd.cfg \
      --include-slow --include-manual \
      --baseline platform/silabs/core-phase2.json
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ...
  ===== RESULT: 98 passed, 2 failed, 1 n/a =====
  baseline written: platform/silabs/core-phase2.json
```

Image `~/silabs/work/hearth-evt` at `679e3d8`, the same image the Events
section above was measured on: **nothing in the firmware changed for this
run**. Harness at `5c51204`, which the result file's own `fw_repo_head`
records. Device state at the start was the documented bench convention,
factory-fresh with the acceptance composition staged (`AT+MTFRESET`, then
`AT+MTEPCLEAR` / `AT+MTEP=256` / `AT+MTEP=770` / `AT+MTEPAPPLY`); 2.12 returns
it to exactly that at the end, and it was verified there afterwards
(`+MTEP:0,1,0x0100`, `+MTEP:1,2,0x0302`, `+MTFABRICS:0`, `+MTSTATE:1,0`,
`+MTEVTMASK:0x0800003F`). Three chip-tool commissionings, each exit 0 on the
first attempt. `otbr-agent` was `active` before and after and was never
started, stopped or reconfigured; the dataset reached chip-tool through the
harness's own D-Bus route and appears nowhere in the run log or the result
file.

**101 rows, the same row set the C6's Thread arm records** in
`test/baselines/thread-lifecycle.json` (100 passed, 1 N/A there).

### Every `+MTEVT` row passed

These are the eight steps `TESTING.md` section 7 lists as event-asserting, and
they are the reason this round exists. Verdicts are from the run above.

| Step | Event rows | Verdict |
|---|---|---|
| 2.1 factory fresh | `+MTEVT:0` after the `AT+MTRESET` reboot | PASS |
| 2.3 commission | `+MTEVT:1`; `+MTEVT:3`; exactly one `+MTEVT:4` after the 3, with 10 s of quiet; `+MTEVT:28` with a legal role token during the join; `+MTEVT:25` during the join | PASS (6 rows) |
| 2.7 second fabric | `+MTEVT:0` on the host-opened window; `+MTEVT:1`; `+MTEVT:3`; exactly one `+MTEVT:4` after the 3 | PASS (4 rows) |
| 2.8 warm reboot | no `+MTEVT:0` on a commissioned device's boot (a negative) | PASS |
| 2.10 window expiry | `+MTEVT:0`; `+MTEVT:4` at the end of an unattended 180 s window; no duplicate close | PASS (3 rows) |
| 2.11 two resets | `+MTEVT:3` on the re-commission | PASS |
| 2.12 rig restore | `+MTEVT:0` after the factory reset | PASS |
| 2.14 transport switch | `+MTEVT:27` on a mismatched boot | **not applicable**, self-declared |

The 2.8 negative deserves its own line because round 2's note warned it would
"pass for the wrong reason" on a silent arm: it does not now. The device raises
`+MTEVT:0` on every window that really opens in this run (2.1, 2.7, 2.10,
2.12) and does not raise one on the commissioned boot in 2.8, so the row is
measuring what it claims to.

2.14 needed no harness change. `AT+MTTRANSPORT?` answers `+MTERR:8` on this
single-transport image, and `main()` reads that as "not the combined image"
straight from the device, so the step reports itself through
`Suite.not_applicable()` exactly as it does on the nRF and the C6's Thread
image.

### The two failing rows, and why they are one fact

| Row | Assertion | Cause |
|---|---|---|
| `2.8 attribute value survived (B63 guard)` | `AT+MTATTR=1,6,0` reads `+MTATTR:1,6,0,1` after a warm SWD reboot of a value written 1 before it | **port defect**, below |
| `2.9 value survived cold boot (B63 guard)` | the same read after a true power cycle | the same defect, the other reboot path |

**A dynamic endpoint's attribute values do not survive any reboot on this
port.** Reproduced outside the harness twice: write `AT+MTATTR=1,6,0,1`, read
back `1`, reset the module over SWD, read `+MTATTR:1,6,0,0`.

The cause is in the dynamic-endpoint mechanism, not in a flag:

- Every attribute on every dynamic endpoint is `EXTERNAL_STORAGE`
  (`port/mt_devtypes_sl.cpp`, "THE STANDING CONDITION"), so CHIP holds no value
  bytes and `emberAfExternalAttribute{Read,Write}Callback` serve every read and
  write out of the endpoint arena's RAM.
- CHIP's write path does call `emAfSaveAttributeToStorageIfNeeded()`
  unconditionally (`app/util/attribute-table.cpp:467`), so a row marked
  `NONVOLATILE` would be written to the attribute persistence provider.
- Nothing reads it back. `emAfLoadAttributeDefaults()` is never called for a
  dynamic endpoint at all (`emberAfSetDynamicEndpoint()` goes to
  `initializeEndpoint()`, which runs cluster init and nothing else), and where
  it does run it discards the stored bytes for an `EXTERNAL` row
  (`attribute-storage.cpp:1322`, `if (!am->IsExternal())`).

So the fix is a **restore path in the port**, next to `rebuild_composition()`
and before `+MTREADY`, plus a ruling on which attributes carry the Matter
spec's N quality and a write-churn budget against the NVM3 soak row that is
already open. It is deliberately not made here: the nRF arm's
`platform/nrf54l15/port/mt_devtypes_zephyr.cpp` declares the identical
`onOffAttrs` table through the identical external-storage mechanism and
contains no `NONVOLATILE` either, so this is **one gap for both Thread arms**,
not a Silabs porting omission, and it will fail the same two rows on the nRF.
Listed under "What round 2 leaves open".

The C6 passes these rows because esp-matter creates the light through its own
endpoint machinery with real attribute storage behind it; B63 itself was a
different bug on that arm (a `StartUpOnOff` default of 0 overwriting a
persistence path that worked).

### What the run measured on the way past

**The 2.8 reboot happens twice here**, and that is the carrier, not a fault.
`relink()` closes the port, runs openocd, and reopens; opening this CDC resets
the module on its own (see "Recovery semantics"), so the console shows two
complete boots 1.2 s apart. Both are warm: the fabric, the dataset and the
composition survive, which is what the row asserts.

**The cold boot in 2.9 is real.** The module is powered from the carrier's
USB, so cutting the hub port cuts the MGM240P too: SWD stops answering
(`Error connecting DP: cannot read IDR`) for as long as the port is down.

Boot figures from the run's console, the Debug Probe's UART CDC at 115200 with
DTR asserted:

| Boot | Free heap at `+MTREADY` | Endpoint arena | NVM3 |
|---|---|---|---|
| factory-fresh, composition of 2 | 95 304 B | 352 of 3 072 B, 2 of 16 endpoints live | 13 objects, 708 B free of 40 960 B, erase count 5 |
| commissioned (2.8's reboots, 2.9's cold boot) | 95 256 B | 352 of 3 072 B | 38 objects, 5 724 B then 4 508 B free, erase count 5 |

The 48 B between the two heap rows is the fabric; the NVM3 free figure is
`availableMemory` at that instant and moves with the repack, which is the same
caveat the soak row carries.

### The bench limit this run found, and its recipe

The first attempt at this run **failed 2.9 and lost the six rows after it**.
`operator_power_cycle()` waits for the `/dev/serial/by-id` path to disappear
before it counts the cycle as having happened, and on this bench's hub a
per-port `uhubctl -l 3-1.3 -p 3 -a off` cuts power (the module goes dark, SWD
stops answering) **without the kernel ever seeing the disconnect**: the device
node stays, stale, and every read through it returns `EPIPE`. Worse, the
matching `-a on` does not bring the port back, so the carrier stays dark.

The recipe that works, and the one to use at the 2.9 prompt:

```bash
uhubctl -l 3-1.3 -a cycle -d 5      # ALL ports of the hub, not -p 3
```

That produces a real disconnect and reconnect: the path vanishes for about
half a second and the harness's poll catches it. It also drops both Debug
Probes for the duration, so a console capture has to be restarted afterwards.
Cycling only the CPico's port does not, and `-a on` on that port alone will
not recover it either; the same all-ports cycle is what brought the carrier
back. The hub is on `3-1.3` and the CPico is its **port 3** (port 1 a Pico 2,
port 2 the other Debug Probe, port 4 the MGM240P's probe and console).

This is a bench fact, not a harness allowance and not a port defect: the
harness's observed-rather-than-trusted power cycle is right, and it is the hub
that lies.

## Measured

Ten sets of figures live here. **"Catalogue batch 2" immediately below is the
current image**, the batch's bench proof, its two phase re-runs and its memory
record: it is catalogue batch 1's image plus six device types, five cluster
servers and a wider endpoint arena. "Catalogue batch 1" after it was the
current image until this batch: round 2 task 7's image plus twelve device
types, three cluster servers and a wider endpoint arena. "Round 2 task 7"
after that is the round's own bench acceptance,
task 6's image plus one NVM3 boot line, commissioned onto the bench fabric
twice, and it is what the batch's figures are measured against.
"Round 2 task 6" after that is the first image whose attributes are readable and
writable over AT, and is what task 7's figures are measured against. "Round 2
task 5" after that is the same image with dynamic endpoints but no attribute
bridge, which is what task 6's figures are measured against. "Round 2
task
4" after that is the same image with the commissioning, network and Thread
surface answering from the stack but every device-type function still a stub,
which is what task 5's figures are measured against.
"Round 2 task 3" after that is the same image with the stack running but every
`mt_matter.h` entry point still stubbed, which is what task 4's figures are
measured against; "Round 2 task 2" is the stack linked but not started;
"Round 2 baseline" is the skeleton on
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

### Catalogue batch 2: the fabric proof, Phase 1 and 2 and the batch's memory record

The current image, 2026-09-24, built from the committed tree at `a271a3f` in
`~/silabs/work/hearth-matter-b2`, a fresh `slc generate` with a `make clean`
inside it, **0 warnings**, which is task 2's directory. Batch 1's record noted
that `hearth.slcp` and `hearth.zap` had not changed since its task 2; this
batch's task 1 changed both (five cluster servers, five components, the
regenerated `zap-generated/`), so a fresh generate was needed, and it was run.
`otbr-agent` was `active` before and after every step here and was never
started, stopped or reconfigured.

All three result files' `fw_repo_head` reads `c6bd3a0`, the committed tip
after task 3, not the built commit. That is not a mismatch: task 3 committed
two test-only changes (`ad4bb57`, `c6bd3a0`) after the image was built, so the
image on the module is `a271a3f`'s bytes and the tree at run time was two
commits past it, both under `test/`. `git status --porcelain` was empty at the
build, and nothing under `platform/` has changed since, so the batch image is
the batch image.

#### The proof: 72 passed, 0 failed, 2 not applicable

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_catalogue_proof.py --port "$MT_PORT" --bridge cpico \
      --batch mg24-batch2 --openocd-config platform/silabs/mg24-swd.cfg \
      --baseline platform/silabs/core-batch2.json
  mg24-batch2: 72 passed, 0 failed, 2 not applicable
  baseline written: platform/silabs/core-batch2.json
```

First run, 2026-09-24 23:03 UTC, on the `a271a3f` image. Seven endpoints
staged, applied, read back exactly, and commissioned in one chip-tool
pairing: the standard on/off light, a colour temperature light, an extended
colour light, a thermostat, a fan, a window covering and an air quality
sensor, the batch's seven-endpoint proof composition. Every one of the six
types answers its `descriptor read device-type-list` with its own id and its
own revision, and the controller-write types carry the whole round trip as
well: the `AT+MTATTR` write is echoed, the controller read answers at that
value, the chip-tool command exits 0, and the second AT read agrees.

The two N/A rows are the two read-only rows: the thermostat's
`LocalTemperature` and the air quality sensor's controller write, recorded by
name rather than silently skipped.

**The air quality echo row passed, and its verdict is the measurement DE531
waits for: the MG24 echoes.** The AT write of the air quality sensor's
measured value returned `+MTATTR:7,91,0,3` on the AT link. That is the
behaviour the design spec's echo ruling (DE531) was written to resolve, and
the MG24 behaves unlike the C6 here: the C6's Thread arm does not echo an AT
attribute write, the MG24 does, and task 5's amendment of `AT_MT_SPEC.md`
section 3.25 takes this line as its evidence.

Two more measurements the proof made on the way past. The thermostat's null
read answered `+MTERR:5`, and a `-500` write round-tripped: the signed path
works on this port, as the unsigned rejection row in Phase 1 already said.
And the window covering's `go-to-lift-percentage` raised `+MTATTR` on
`Target` (0x000B), not `Current`: the command writes the target, and `Current`
moves only as the (absent) mechanism reports it, which is what the row's
expectation encodes.

#### Harness Phase 1, re-run on the batch image: 292 passed, 4 failed

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 1 \
      --baseline platform/silabs/core-phase1.json
  ===== RESULT: 292 passed, 4 failed =====
```

**The same four rows.** `git diff` on the result file is the header's
`fw_repo_head` and timestamp only; all 296 verdicts are unchanged, and the
four failing rows are task 6's four parked rows, the two unported-type
compositions (catalogue batches 7a and 7b, the EVSE round). Batch 1's record
stated that Phase 1 was not re-run on `ca05afa` and was not expected to move;
this run is that statement measured rather than assumed, on the batch image,
and nothing moved.

#### Harness Phase 2, re-run on the batch image: 98 passed, 2 failed, 1 n/a

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 2 \
      --openocd-config platform/silabs/mg24-swd.cfg \
      --include-slow --include-manual \
      --baseline platform/silabs/core-phase2.json
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ===== RESULT: 98 passed, 2 failed, 1 n/a =====
  baseline written: platform/silabs/core-phase2.json
```

**Row-identical to the committed file**, the `git diff` being the header's
`fw_repo_head` and timestamp again: all 101 verdicts are unchanged, including
the two B512 rows (2.8 and 2.9's `attribute value survived (B63 guard)`) and
2.14's self-declared N/A. Three chip-tool commissionings, each exit 0 on the
first attempt; the dataset reached chip-tool through the harness's own D-Bus
route and appears nowhere in the run log or the result file. 2.9's cold boot
used the hub recipe, `uhubctl -l 3-1.3 -a cycle -d 5` ("The bench limit this
run found").

One bench fact from the run itself: the CPico's carrier was found **unenumerated
before task 1**, and a port-3 cycle did not bring it back; a whole-hub cycle,
`uhubctl -l 3-1.3 -a cycle -d 5`, did. The hub-lies fact of the section named
above, confirmed once more, from the bench rather than from the recipe.

#### Flash and RAM

`arm-none-eabi-size` on `~/silabs/work/hearth-matter-b2/build/debug/hearth.out`,
2026-09-24, at `a271a3f`, against catalogue batch 1's row:

| | Catalogue batch 2 (`a271a3f`) | Catalogue batch 1 (`ca05afa`) | Delta |
|---|---|---|---|
| `text` (`size`) | 889 656 | 857 928 | **+31 728** |
| `.bss` (`-A`) | 131 704 | 122 040 | **+9 664** |
| `.memory_manager_heap` | 121 976 | 131 656 | -9 680 |

The `.bss` rise splits exactly in two, and both halves were measured inside
this round rather than apportioned afterwards:

| Half | `.bss` | Where it was measured |
|---|---|---|
| the five cluster servers (`matter_color_control`, `matter_thermostat`, `matter_fan_control`, `matter_window_covering`, `matter_air_quality`) | **+5 440** | task 1's build, commit `d10d307`: 127 480 against batch 1's 122 040 |
| the endpoint arena, 5 376 to 9 600 | **+4 224** | task 2's build, commit `a271a3f`: 131 704 against task 1's 127 480, and 9 600 - 5 376 = 4 224 exactly |

So the six device types' own metadata again costs **no `.bss` at all**: every
new table is `const` and lands in flash, the same as batch 1's. And again every
byte of the `.bss` rise comes straight out of `.memory_manager_heap`: the
linker hands the heap section whatever RAM is left over, and it gives back
9 680 B here against 9 664 B of `.bss`, the 16 B difference being this bench's
boot-to-boot spread, the same spread batch 1's record named against its own
standings.

#### The arena

`HEARTH_EP_ARENA_BYTES` is **9 600** = 16 x 600, sixteen blocks of the widest
declared type (the extended colour light, 600 B of arena cost after
`hearth_arena_cost()`'s rounding). Batch 1's ruling had been 16 x 336 = 5 376;
this batch's six types made the block wider and the batch kept the strong
promise by raising the arena to sixteen of the new widest block, ruling DE530,
rather than lowering the floor. The strong promise is the same: every
composition the image accepts, it builds, with nothing left over and nothing
wasted, and it is proven below rather than assumed.

Read off this image's console, all three lines from this session:

```
I devtypes: endpoint arena: 2112 of 9600 B handed out, 7488 B free, 7 of 16 serviceable endpoints live
I devtypes: endpoint arena: 9600 of 9600 B handed out, 0 B free, 16 of 16 serviceable endpoints live
I devtypes: endpoint arena: 352 of 9600 B handed out, 9248 B free, 2 of 16 serviceable endpoints live
```

In order: the batch's seven-endpoint proof composition (the on/off light, the
colour temperature light, the extended colour light, the thermostat, the fan,
the window covering, the air quality sensor), task 2's sixteen-`0x010D` smoke,
and the bench's standard two restored at the end.

**2 112 of 9 600** matches the per-type arithmetic term for term, the figure
task 2's smoke measured before this run: 192 + 536 + 600 + 256 + 176 + 224 +
128. At seven endpoints, the widest composition this batch's proof stages, the
arena is 22 % used.

**The strong promise, DE530, is proven, not assumed.** Task 2's smoke staged
sixteen extended colour lights, the sixteen widest blocks the arena holds, and
the console read **9 600 of 9 600 B handed out, 16 of 16 serviceable endpoints
live**: every endpoint the arena can hold is built and serviceable, with
nothing left over and nothing wasted. That is the promise the design spec
ruling names, measured on this image.

**The second arena still does not exist.** The cluster-object arena arrives
with the first delegate-bearing device type; nothing in this batch needs one,
and `hearth_arena` in `port/mt_dyn_store.h` is still the allocator both will
use.

#### Free heap at `+MTREADY`, and NVM3

From the boot task's own line, this session:

| Boot | Composition | Free heap at `+MTREADY` | NVM3 |
|---|---|---|---|
| factory-fresh | 0 endpoints | **81 296 B** | not read this session |
| commissioned, one fabric | the standard 2 | **81 248 B** | not read this session |

The factory-fresh figure is the same on every boot of the proof and of Phase 1,
any composition; the commissioned figure is Phase 2's, the standard
`0x0100,0x0302` composition with one fabric. `availableMemory` was not read
this session, and the NVM3 soak row's standing caveat applies unchanged; this
table records the heap only.

Two things to say beside the figures. **The fabric costs 48 B of heap held at
`+MTREADY`** here too, 81 296 against 81 248: the same 48 B round 2 task 7 and
batch 1 measured, the figure a property of the fabric, not of the composition.
And **the commissioned figure is not the same measurement as batch 1's**:
batch 1's 90 928 B was taken with its thirteen-endpoint composition, this one
with the standard two, so the two are not comparable beyond the fabric cost
itself, which is the only thing the comparison is used for here. Against
batch 1's 90 976 B the standing factory-fresh figure is 9 680 B lower, of which
9 664 B is `.memory_manager_heap` shrinking to pay for this batch's `.bss`;
the remaining 16 B is this bench's boot-to-boot spread, the same spread batch
1's record named.

The `+MTREADY` figure is still a floor measured at the worst moment of the
boot, with `hearth_boot_task`'s own 5 120-byte stack and CHIP's init transients
outstanding; round 2 task 7's note on the steady-state figure applies
unchanged.

#### Bench state found and left

**Found**, before anything was run, factory-fresh with the standard
composition staged, `0x0100,0x0302`, which is the state every run in this
batch both expects and restores. The carrier itself had to be brought back
first (the paragraph above).

**Left** the same way, asserted three times independently: by the proof
script's own restore rows, by Phase 2's 2.12, and by the measurement cycle at
the end. The arena line for the restored composition is the third one above,
352 of 9 600.

No Thread operational dataset was printed, logged or written at any point: the
harness's D-Bus route hands it to chip-tool's argv and nowhere else, and the
result files and this section name only commit ids and the CPico's serial.

### Catalogue batch 1: the fabric proof, Phase 2 and the batch's memory record

The current image until this batch, 2026-09-22, built from the committed tree
at `ca05afa` in
`~/silabs/work/hearth-matter-b1`, which is task 2's directory. Nothing in
`hearth.slcp` or `data_model/hearth.zap` has changed since task 2, so no fresh
`slc generate` has been needed since: every build in this batch is an
incremental `make` inside that one generated tree. `otbr-agent` was `active`
before and after every step here and was never started, stopped or
reconfigured.

```
$ git status --porcelain                                (clean, at ca05afa)
$ source platform/silabs/toolchain.env
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-b1 \
      -f hearth.Makefile -j8
$ cd build/debug && commander gbl create hearth.gbl --app hearth.s37
$ python3 platform/silabs/fw/flash.py --port "$MT_PORT" --image .../hearth.gbl
  6730 blocks, 6730 frame(s) sent (0 retransmit(s)), 95.6 s
  +MTREADY seen: the module is running the new image
```

`hearth.bin` 861 328 B, md5 `276968b351871f828cd5c523f9cd555a`; `hearth.gbl`
861 400 B, md5 `fa2837e6f13c9df532ad9b8012991931`. Every figure and every run
below is this image. Both result files' `fw_repo_head` reads `ca05afa`, the
built commit exactly.

Two things about that transcript, because the `make` in it compiled nothing.
The build directory was already at `ca05afa` when this record was taken, so
the repeated incremental `make` had nothing to do, which is exactly what says
the tree and the directory agree; the module was reflashed from it anyway, so
what is on the bench is beyond doubt. And because a `make` that compiles
nothing proves nothing about warnings, the one file `ca05afa` changed was
recompiled deliberately afterwards and the image relinked:

```
$ touch platform/silabs/port/mt_devtypes_sl.cpp
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-b1 \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-b1/build-ca05afa.log
  Building .../platform/silabs/port/mt_devtypes_sl.cpp
  Linking build/debug/hearth.out
$ grep -ci warning    ~/silabs/work/hearth-matter-b1/build-ca05afa.log     0
$ grep -c  deprecated ~/silabs/work/hearth-matter-b1/build-ca05afa.log     0
$ md5sum build/debug/hearth.bin   276968b351871f828cd5c523f9cd555a
$ md5sum build/debug/hearth.gbl   fa2837e6f13c9df532ad9b8012991931
```

**Warning-free and deprecation-free, and byte-identical to the image on the
bench**, which re-proves F497's relink rule on this build as well as the
warning count.

#### The proof: 75 passed, 0 failed, 9 not applicable

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_catalogue_proof.py --port "$MT_PORT" --bridge cpico \
      --batch mg24-batch1 --openocd-config platform/silabs/mg24-swd.cfg \
      --baseline platform/silabs/core-batch1.json
  mg24-batch1: 75 passed, 0 failed, 9 not applicable
  baseline written: platform/silabs/core-batch1.json
```

Thirteen endpoints staged, applied, read back exactly, and commissioned in one
chip-tool pairing. Every one of the twelve types answers its
`descriptor read device-type-list` with its own id and its own revision, takes
an `AT+MTATTR` write of its primary attribute and is read back at that value
by the controller, and agrees with a second AT read afterwards. The nine N/A
rows are the nine read-only clusters' controller-write rows, recorded by name
rather than silently skipped. The three types with a controller-side path
(0x0101, 0x010A and 0x010B) carry the whole quadruple as well: the chip-tool
command exits 0, the `+MTATTR` URC arrives on the AT link with the new value,
the controller reads that value back, and the second AT read agrees. What the
script proves and how to run it for a later batch is in the docs repository,
`TESTING.md` section 8.

**Both rows the design spec named as risks passed.** The four BooleanState
sensors' `controller reads state-value = True after the AT write` rows pass, so
`FindClusterOnEndpoint()` does resolve a dynamic endpoint's object in this tree
and the bridge in `MatterPostAttributeChangeCallback` works; the contact
sensor's `state-change event present` row passes, so `SetStateValue()` emits
the cluster's event too. The occupancy sensor's `feature-map = 2` row passes,
so `matter_occupancy_sensor` constructs no `Instance` here and the seed row is
still the only writer of that cluster's metadata.

#### The six rows that failed first, and the defect behind them (B525)

**The first recorded run of this proof, on the `d263b6b` image one code commit
earlier, was 69 passed, 6 failed, 9 not applicable.** The six were one fact on
two device types, three rows each: 0x0101 dimmable light on endpoint 2 and
0x010B dimmable plug-in unit on endpoint 13.

(It was the second run on the bench. The first, on the same image, was 66
passed, 9 failed, 9 not applicable, and three of those nine were the script's
own row table rather than the port: an `on-off` read parsed as an integer, an
`on` command that asked for the value the AT write had already set, and a
`move-to-level` that Options processing refuses with SUCCESS while `OnOff` is
FALSE. `b1eb3ad` corrected all three, with self-tests, and that run's result
file was overwritten by the next one before anything was committed. The rules
it produced are in the docs repository, `TESTING.md` section 8.)

| Row, on both endpoints | Then | Now |
|---|---|---|
| `+MTATTR:<ep>,8,0,200 on the AT link` | FAIL | PASS |
| `controller reads back 200` | FAIL | PASS |
| `second AT read agrees (200)` | FAIL | PASS |

The three rows before them passed even then, which is what bounded the defect:
the device-type list carried `(257, 3)` and `(267, 5)`, an `AT+MTATTR` write of
`CurrentLevel` 100 was read back as 100 by the controller, and
`move-to-level-with-on-off 200` exited 0. The attributes were served, the
endpoint was real, the command reached the cluster server and the server
accepted it. What did not happen was the move.

Reproduced by hand on endpoint 2, 2026-09-22, commissioned, before the fix:

```
chip-tool levelcontrol read min-level      ep2 -> 1
chip-tool levelcontrol read max-level      ep2 -> 254
chip-tool levelcontrol read options        ep2 -> 0
chip-tool onoff        read on-off         ep2 -> FALSE
> AT+MTATTR=2,8,0,100
  +MTATTR:2,8,0,100
chip-tool levelcontrol move-to-level-with-on-off 200 0 0 0 ep2      (exit 0)
  the only URC raised: '+MTATTR:2,8,0,0'
> AT+MTATTR=2,8,0
  +MTATTR:2,8,0,0
chip-tool levelcontrol read current-level  ep2 -> 0
```

**`CurrentLevel` goes to 0, which is below the `MinLevel` of 1 the same
endpoint advertises.** The console says the rest on the way past:

```
I chip: [ZCL] RX level-control: MOVE_TO_LEVEL_WITH_ON_OFF c8  0 0 0
I chip: [ZCL] Setting on/off to OFF due to level change
I chip: [ZCL] Endpoint 2 On/off already set to new value
```

The mechanism is the occupancy init callback's, in a cluster that cannot work
around it from a seed row. `level-control.cpp` keeps a private
`EmberAfLevelControlState` per endpoint in `stateTable`, sized
`MATTER_DM_LEVEL_CONTROL_CLUSTER_SERVER_ENDPOINT_COUNT +
CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT`, so a dynamic endpoint does get a
slot and `getState()` answers non-null. The only writer of that struct is
`emberAfLevelControlClusterServerInitCallback()`, and that is a per-cluster
init function: `DECLARE_DYNAMIC_CLUSTER` hardcodes `.functions = NULL`, so it
never runs for an endpoint this port creates, exactly as
`port/mt_devtypes_sl.cpp` already records for OccupancySensing. The struct
therefore keeps its static zero initialisation, `state->minLevel` and
`state->maxLevel` are both **0**, and `moveToLevelHandler()` clamps against
them:

```cpp
if (state->maxLevel <= level) { state->moveToLevel = state->maxLevel; }
```

`0 <= 200`, so the target becomes 0 and the transition runs the level down
instead of up. `MinLevel` and `MaxLevel` read correctly over the wire because
those are ember attributes living in the endpoint arena; the state struct is a
second, private copy that only the init callback ever fills. OccupancySensing
survives the same gap because its seed row writes the attributes and ember
serves them; LevelControl cannot, because the value that decides the move is
not an attribute.

**Classification: a port defect, graph B525, and one both Thread arms have by
construction.** The nRF arm declares the same LevelControl tables through the
same `DECLARE_DYNAMIC_CLUSTER` macro with the same null functions array, so
its dimmable light behaves identically; its own batch 1 proved the types in a
prose bench session and never sent a level command. **The nRF half of B525 is
open.** It was not the script's: the script sends the only command that can
act on a cold composition (the "without On/Off" variants are refused with
SUCCESS while `OnOff` is FALSE), and these rows passed unchanged once the
state was initialised. It was not the SDK's either: a cluster with an init
function is entitled to have it called, and this port was creating endpoints
by a route that called none.

**Fixed in `ca05afa`**, which is the image everything above and below is
measured on. The hook is one level up from the occupancy seed's:
`emberAfClusterInitCallback()` runs for every cluster of every endpoint,
dynamic ones included, one line before ember looks for the functions array it
will not find, and the generated dispatch sends LevelControl's to a weak
`emberAfLevelControlClusterInitCallback()`. `port/mt_devtypes_sl.cpp` now
defines that strongly, which is the same hook the nRF arm already uses for its
DoorLock init:

```cpp
void emberAfLevelControlClusterInitCallback(EndpointId endpoint)
{
    if (endpoint == kCatalogueEndpointId) {
        return;
    }
    emberAfLevelControlClusterServerInitCallback(endpoint);
}
```

Endpoint 240 returns at once, because it is ZAP-declared and ember runs its
server init on the next line anyway. The seeds already agree with what the
init reads and writes, so nothing moves at boot and no `+MTATTR` fires. OnOff
needs no equivalent and the file's comment says why: its init body is the
`StartUpOnOff` power-up behaviour, it caches nothing, and every one of its
attributes is an ember attribute this port declares.

The same probe on the fixed image, endpoint 2, commissioned:

```
> AT+MTATTR=2,8,0,100
  +MTATTR:2,8,0,100
chip-tool levelcontrol move-to-level-with-on-off 200 0 0 0 ep2      (exit 0)
  URCs raised: '+MTATTR:2,6,0,1', '+MTATTR:2,8,0,200'
> AT+MTATTR=2,8,0
  +MTATTR:2,8,0,200
chip-tool levelcontrol read current-level  ep2 -> 200
chip-tool onoff        read on-off         ep2 -> TRUE
```

```
I chip: [ZCL] RX level-control: MOVE_TO_LEVEL_WITH_ON_OFF c8  0 0 0
I chip: [ZCL] Setting on/off to ON due to level change
```

ON where it said OFF, the target reached instead of clamped, and the command's
own `WithOnOff` half visible as the second URC. The cost is **+10 B of `.text`
for the new function against the 2 B weak stub it replaces**, absorbed by
inter-object alignment: the section totals below are unchanged to the byte.

#### Harness Phase 2, re-run on the batch image: 98 passed, 2 failed, 1 n/a

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 2 \
      --openocd-config platform/silabs/mg24-swd.cfg \
      --include-slow --include-manual \
      --baseline platform/silabs/core-phase2.json
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ===== RESULT: 98 passed, 2 failed, 1 n/a =====
  baseline written: platform/silabs/core-phase2.json
```

**Identical to 2026-09-21's run, row for row.** `git diff` on the result file
is two lines, `fw_repo_head` and `timestamp`; all 101 verdicts are unchanged,
including the two B512 rows (2.8 and 2.9's `attribute value survived (B63
guard)`) and 2.14's self-declared N/A. So the twelve device types, the three
cluster servers and the wider arena cost the lifecycle chain nothing. Three
chip-tool commissionings, each exit 0 on the first attempt; the dataset
reached chip-tool through the harness's own D-Bus route and appears nowhere in
the run log or the result file. 2.9's cold boot used this bench's hub recipe,
`uhubctl -l 3-1.3 -a cycle -d 5` ("The bench limit this run found").

**Phase 1 was not re-run on this image, and its 292 of 296 is task 3's
measurement**, taken on the earlier `f4ddd4f` build (`hearth.bin` md5
`fb3a1efc...`, the same bytes as `d263b6b`), with the same four unported-type
rows failing as `core-phase1.json` records. It is not expected to move on
`ca05afa`: the only change is a strong
`emberAfLevelControlClusterInitCallback()` that runs for dynamic endpoints
carrying LevelControl and returns at once for endpoint 240, and Phase 1 stages
none of the batch-1 device types at all. Stated rather than assumed, so a
reader of this section does not take the figure for a `ca05afa` run.

*Superseded for Phase 1, by the "Catalogue batch 2" section above: that
image's run re-ran Phase 1 on the batch image and took the header-only diff,
the same 292 of 296 with the same four rows. This section's statement stands
for `ca05afa` itself, which was never Phase-1-ran.*

#### Flash and RAM

`arm-none-eabi-size` on `~/silabs/work/hearth-matter-b1/build/debug/hearth.out`,
2026-09-22, at `ca05afa`, against round 2 task 7's row:

| | Catalogue batch 1 (`ca05afa`) | Round 2 task 7 (`a9d6ae6`) | Delta |
|---|---|---|---|
| `text` (`size`) | 857 928 | 845 340 | +12 588 |
| `data` (`size`) | 3 384 | 3 368 | +16 |
| `bss` (`size`, includes the heap section) | 258 308 | 258 324 | -16 |
| `.text` (`-A`) | 857 092 | 844 504 | +12 588 |
| `.bss` (`-A`) | 122 040 | 117 720 | **+4 320** |
| `.memory_manager_heap` | 131 656 | 135 992 | -4 336 |

The `.bss` rise splits exactly in two, and both halves were measured inside
this round rather than apportioned afterwards:

| Half | `.bss` | Where it was measured |
|---|---|---|
| the three cluster servers (`matter_level_control`, `matter_boolean_state`, `matter_occupancy_sensor`) plus the seven clusters enabled on endpoint 240 | **+2 016** | task 2's build, commit `70d2c1b`: 119 736 against task 7's 117 720, with no port change in it yet |
| the endpoint arena, 3 072 to 5 376 | **+2 304** | task 3's build, commit `f4ddd4f`: 122 040 against task 2's 119 736, and 5 376 - 3 072 = 2 304 exactly |

So the twelve device types' own metadata costs **no `.bss` at all**: every new
table is `const` and lands in flash, which is what the `HEARTH_DECLARE_CONST_*`
macros exist for. `.text` splits the same way, +10 592 for task 2's components
and clusters and +1 996 for task 3's tables and the BooleanState bridge. Every
byte of the `.bss` rise comes straight out of `.memory_manager_heap`, which is
the linker handing the heap section whatever RAM is left over.

**Every figure in that table is the same at `ca05afa` as it was at
`d263b6b`**, the commit before the LevelControl fix, and both were measured
rather than assumed: the new function is 10 B and the weak stub it replaces
is 2 B, and the +8 B disappears into the 4-byte alignment padding between
objects. The two `hearth.bin` files differ (md5 `276968b3...` against
`fb3a1efc...`), so this is a measured coincidence of the section totals and
not a stale reading.

`hearth.bin` is **861 328 B**, **55.93 %** of the 1 540 096 B application
region, and still **163 812 B below** the stock Silicon Labs `lighting-app` on
the same SDK and extension (1 025 140 B).

#### The arena

`HEARTH_EP_ARENA_BYTES` is **5 376** = 16 x 336, sixteen blocks of the widest
declared type (the dimmable light and plug: 4 clusters, 20 slots,
4 x 4 + 16 x 20 = 336 B of payload, already 8-aligned, so
`hearth_arena_cost()` adds nothing). The design spec's ruling said
16 x 344 = 5 504; 344 is 336 rounded up a second time, and the build's own
`static_assert` refused it (task 3). The strong promise is unchanged: every
composition the image accepts, it builds, with nothing left over and nothing
wasted.

Read off this image's console, all three lines from this session:

```
I devtypes: endpoint arena: 0 of 5376 B handed out, 5376 B free, 0 of 16 serviceable endpoints live
I devtypes: endpoint arena: 2368 of 5376 B handed out, 3008 B free, 13 of 16 serviceable endpoints live
I devtypes: endpoint arena: 352 of 5376 B handed out, 5024 B free, 2 of 16 serviceable endpoints live
```

Factory-fresh, the batch's thirteen-endpoint proof composition, and the
bench's standard two. **2 368 of 5 376** matches the per-type arithmetic term
for term: 2 x 336 (the two dimmables) + 2 x 192 (the two on/off types)
+ 4 x 128 (the four boolean-state sensors) + 5 x 160 (occupancy and the four
measurement sensors). At thirteen endpoints, the widest composition this batch
can build, the arena is 44 % used, so the sixteen-block tier is nowhere near
binding.

**The second arena still does not exist.** The cluster-object arena arrives
with batch 2's first delegate-bearing device type; nothing in this batch needs
one, and `hearth_arena` in `port/mt_dyn_store.h` is still the allocator both
will use.

#### Free heap at `+MTREADY`, and NVM3

From the boot task's own line, this session, all four on this image:

| Boot | Composition | Free heap at `+MTREADY` | NVM3 |
|---|---|---|---|
| factory-fresh | 0 endpoints | **90 976 B** | 11 objects, 304 B free of 40 960 |
| uncommissioned | the batch's 13 | **90 976 B** | 12 objects, 176 B free |
| **commissioned, one fabric** | the batch's 13 | **90 928 B** | 37 objects, 908 B free |
| uncommissioned | the standard 2 | **90 976 B** | 12 objects, 316 B free |

Two things worth keeping. **The fabric costs 48 B of heap held at `+MTREADY`**,
90 976 against 90 928, which is the same 48 B round 2 task 7 measured with two
endpoints: the figure is a property of the fabric, not of the composition. And
**the endpoint count costs the heap nothing at all**: 0, 2 and 13 endpoints all
report 90 976 uncommissioned, because an endpoint is `.bss` arena and not
shared heap. Against task 7's 95 336 / 95 288 the standing figures are 4 360 B
lower, of which 4 336 B is `.memory_manager_heap` shrinking to pay for this
batch's `.bss`; the remaining 24 B is this bench's boot-to-boot spread.

The `+MTREADY` figure is still a floor measured at the worst moment of the
boot, with `hearth_boot_task`'s own 5 120-byte stack and CHIP's init transients
outstanding; round 2 task 7's note on the steady-state figure applies
unchanged. `availableMemory` remains the log-structured figure NVM3 reports
rather than `size - used`, with the same repack caveat the soak row carries;
the erase count moved 5 to 8 across this day's resets, which is the repack
running three times. **The four figures above are a pre-repack trough and the
lowest this bench has recorded, 176 B at the bottom**, against 480 B on the
round 2 task 7 session. Nothing failed and no write was refused, so this is
the mechanism working rather than an exhaustion, but it is the same fact the
NVM3 soak row owns and it now has a smaller number in it.

#### Bench state found and left

**Found**, before anything was flashed, over the harness's own `ATLink` with
`open_at_port(..., bridge="cpico")`: `+MTREADY`, `+MTVER:1.2.0`,
`+MTEP:0,1,0x0100` and `+MTEP:1,2,0x0302`, `+MTFABRICS:0`, `+MTSTATE:1,0`,
`+MTNET:THREAD,0,0,0`. Factory-fresh with the standard composition staged,
which is the state every run in this batch both expects and restores.

**Left** the same way, asserted three times independently: by the proof
script's own restore rows, by Phase 2's 2.12, and by the measurement cycle at
the end.

```
> AT+MTEP?
  +MTEP:0,1,0x0100
  +MTEP:1,2,0x0302
  OK
> AT+MTFABRICS?
  +MTFABRICS:0
> AT+MTSTATE?
  +MTSTATE:1,0
> AT+MTNET?
  +MTNET:THREAD,0,0,0
```

No Thread operational dataset was printed, logged or written at any point: the
harness's D-Bus route hands it to chip-tool's argv and nowhere else, and every
file this section names was scanned for a hex run of 24 characters or more,
which matches the commit ids and nothing else.

### Round 2 task 7: the bench acceptance, and the round's memory record

The current image, 2026-09-18, built from the committed tree at `a9d6ae6` in a
fresh `~/silabs/work/hearth-matter-t7` by the "Building" recipe above, flashed
with `fw/flash.py` over XMODEM. The only source change since task 6's fix round
is the NVM3 boot line this section needed; everything else in this section is
measurement, not change. `otbr-agent` was `active` before and after every step
and was never started, stopped or reconfigured.

```
$ git status --porcelain      (clean)
$ slc generate -d ~/silabs/work/hearth-matter-t7 --sdk-package-path "$SISDK_ROOT" \
      --sdk-package-path "$MATTER_EXT_ROOT" -p platform/silabs/hearth.slcp \
      --with MGM240PA32VNA --generator-timeout=180 -o makefile
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t7 \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t7/build.log
$ grep -ci warning   ~/silabs/work/hearth-matter-t7/build.log    0
$ grep -c deprecated ~/silabs/work/hearth-matter-t7/build.log    0
$ cd build/debug && commander gbl create hearth.gbl --app hearth.s37
```

#### Flash

`arm-none-eabi-size` on `~/silabs/work/hearth-matter-t7/build/debug/hearth.out`,
2026-09-18, against task 6's fix-round figures:

| | Task 7 | Task 6 fix round | Delta |
|---|---|---|---|
| `text` | 845 340 | 844 956 | +384 |
| `data` | 3 368 | 3 368 | 0 |
| `bss` (size's, includes the heap section) | 258 324 | 258 324 | 0 |
| `.text` (`-A`) | 844 504 | 844 120 | +384 |
| `.bss` (`-A`) | 117 720 | 117 720 | 0 |
| `.memory_manager_heap` | 135 992 | 135 992 | 0 |

The whole `+384 B` is the NVM3 boot line (`src/main.cpp`'s
`hearth_nvm3_report()`): a format string, three calls and the `nvm3_MemInfo_t`
on the boot task's stack. Nothing in `.bss`, nothing in `.data`, nothing at run
time.

`hearth.bin` **848 724 B**, **55.11 %** of the 1 540 096 B application region,
`md5sum b21e738ab19fbff624bd118dd63175f1`. `hearth.gbl` 848 796 B,
`md5sum 132c1a95d461bf3b11bec654784e70ef`, sent by `fw/flash.py` as 6 632
blocks with 0 retransmits in 94.2 s.

**The md5 identifies this one build and is not reproducible across `slc
generate` runs** (F497, proved with a `cmp` in task 5): OpenThread's version
banner is built from `__DATE__ __TIME__`, so two generations of the same tree
differ in the six bytes of the time-of-day digits at that string and nowhere
else. This task sharpened the caveat by accident and the result is worth
keeping: a **relink** inside one generated tree IS reproducible. The scratch
heap probe below was added, built and flashed in this same directory and then
removed, and the rebuild came back to `md5sum b21e738a...` exactly. The banner
string is compiled once per generation, not once per link.

##### Against the three yardsticks

| Yardstick | Flash | This image |
|---|---|---|
| Round 1 skeleton, no Matter (`text`) | 47 596 B | 845 340 B `text` |
| Stock Silicon Labs `lighting-app`, same SDK and extension | 1 025 140 B | 848 724 B `bin`, **176 416 B below it** |
| nRF54L15 matter-core round, app image | 753 691 B | 848 724 B, **95 033 B (12.6 %) above it** |

The stock light is the yardstick that controls for the SDK generation, and
Hearth is comfortably under it with more of the data model reachable from the
host. The nRF figure is the one the design spec says to explain rather than
accept, so: **it is not explained by anything measured here, and the two are
not the same build in any sense that would make the difference attributable.**
Different silicon vendor, different SDK, different CHIP release (Matter 1.5
through the Silicon Labs extension 2.8.1-1.5 against the nRF arm's NCS build),
different radio stacks (Silicon Labs' own Bluetooth host and controller plus
RAIL multiprotocol, against Nordic's SoftDevice), and this image carries SEGGER
RTT whether or not anything uses it, because `matter_provision_default` is not
optional and requires `iostream_rtt` ("The components, and the three that could
not stay"). One contributor IS named and is this arm's own choice rather than
an accident: the catalogue endpoint compiles a set of cluster servers into the
image that no composition serves yet, and task 3 recorded it as the reason this
arm first crossed the nRF's figure. What IS measured, in this arm's own terms,
is every increment this
round added: tiny printf 3 272 B, ruling F500's carve-out 396 B, the attribute
bridge 1 760 B, the NVM3 line 384 B. A per-component decomposition against the
nRF arm would need both images built from one matched source list and is not
something this round did.

#### RAM

The design spec's section 6 asks for RAM in three parts. On this platform there
are only two pools, not three, and the reason is structural rather than a
measurement gap: `matter_platform_mg`'s replacement chain requires
`freertos_heap_3`, which forwards `pvPortMalloc()` to the C library's `malloc()`
and therefore into `sl_memory_manager`'s one pool ("The heap changed shape").
There is no `configTOTAL_HEAP_SIZE` and no separate FreeRTOS high-water mark to
report.

| Part | Figure | Where from |
|---|---|---|
| Static image | `.bss` 117 720 B + `.data` 3 368 B = **121 088 B** | `arm-none-eabi-size -A` |
| The one heap, reserved | **135 992 B** `.memory_manager_heap` | the same |
| Free at `+MTREADY`, uncommissioned | **95 336 B** | `sl_memory_get_free_heap_size()`, the boot task's own line |
| Free at `+MTREADY`, one fabric stored | **95 288 B** | the same line, on the boot after commissioning |
| Free at the first controller-driven attribute change after commissioning | **104 216 B** | the scratch probe below |
| Free at the second, a few seconds later | **104 160 B** | the same |

Two of those rows need their explanation beside them.

**The fabric costs 48 B of heap held at `+MTREADY`**, 95 336 against 95 288, and
the figure is insensitive to the endpoint count: 0, 1 and 2 endpoints all
report 95 336 uncommissioned, because an endpoint costs `.bss` arena and not the
shared heap (task 5 measured the same thing).

**The post-commissioning figure is HIGHER than the `+MTREADY` figure, by about
8.9 KB, and that is not a mistake.** `+MTREADY` is logged from inside
`hearth_boot_task`, which is still holding its own 5 120-byte stack and TCB and
runs `vTaskDelete(NULL)` a few instructions later, and CHIP's init transients
are still outstanding at that moment. So the `+MTREADY` figure is a floor
measured at the worst moment of the boot, and **104 KB is the steady-state
figure with a fabric, a CASE session and two endpoints live.** Anyone comparing
this platform's free RAM against another arm's should quote the second number
and say which it is.

##### How the post-commissioning figure was measured, and that it is gone

A scratch probe, a `HEARTH_LOGI` at the top of
`MatterPostAttributeChangeCallback` in `port/mt_matter_sl.cpp` printing
`sl_memory_get_free_heap_size()` for the first twelve attribute changes. It was
never committed: built and flashed from a deliberately dirty tree, read, then
reverted, and the rebuild produced a byte-identical `hearth.bin` (md5
`b21e738a...`), which is the evidence that nothing of it is in the shipping
image. Its own cost while it existed was **132 B of `text`** (845 472
against 845 340), measured on the first of the two probe builds.

The first version of the probe fired once and fired at the wrong moment, which
is worth recording because it is a fact about the boot rather than about the
probe:

```
I probe: attr change 1 on ep 240 cluster 0x0004: heap free 101912 B
I probe: attr change 2 on ep 240 cluster 0x0004: heap free 101912 B
I boot: composition rebuilt: 1 endpoint(s)
...
I probe: attr change 3 on ep 1 cluster 0x0006: heap free 104216 B
I probe: attr change 4 on ep 1 cluster 0x0006: heap free 104160 B
```

The first two changes are the generated code-driven init writing Groups
attributes on **endpoint 240**, the catalogue endpoint, before
`hearth_boot_task` disables it. `MatterPostAttributeChangeCallback`'s first line
filters endpoint 0 and `kCatalogueEndpointId` out, so neither reached the AT
link, and this is the first direct evidence on the console that the filter is
load-bearing rather than defensive. Changes 3 and 4 are the two controller
toggles.

#### The arenas

```
I devtypes: endpoint arena: 0 of 3072 B handed out, 3072 B free, 0 of 16 serviceable endpoints live
I devtypes: endpoint arena: 192 of 3072 B handed out, 2880 B free, 1 of 16 serviceable endpoints live
I devtypes: endpoint arena: 352 of 3072 B handed out, 2720 B free, 2 of 16 serviceable endpoints live
```

Those three lines are, in order, the factory-fresh boot, the harness rig (one
on/off light) and the acceptance composition (light plus temperature sensor).
The light costs 192 B and the temperature sensor 160 B, both after
`hearth_arena_cost`'s rounding to 8. At 16 serviceable endpoints the arena has
**2 720 B of 3 072 B still free with the acceptance composition live**, so the
tier is not close to binding on these two types; a catalogue batch with wider
blocks is what will move it.

**The second arena does not exist in this image.** The cluster-object arena is
deliberately absent until the first delegate-bearing device type arrives
(catalogue batch 2; the reasoning is at the end of `port/mt_matter_sl.cpp`, and
`hearth_arena` in `port/mt_dyn_store.h` is already the allocator both will
use). The design spec asks for "the two arenas' occupancy"; there is one, and
this is why.

#### NVM3

The region, from the generated linker file and confirmed against the running
instance:

| | |
|---|---|
| Configured size | `NVM3_DEFAULT_NVM_SIZE 40960` in `hearth.slcp` |
| Placement | `.nvm` is a `DSECT` in `autogen/linkerfile.ld`; `__nvm3Base = linker_nvm_end - SIZEOF(.nvm)` |
| Bounds in the map | `0x08174000` to `linker_nvm_end` `0x0817E000` |
| Against the application region | `0x08006000` + `0x178000` = `0x0817E000`, so NVM3 is the top of it and the image (ending `0x080D5354`) never reaches it |
| Cost to the image | none: a `DSECT` occupies no bytes in `hearth.bin` |
| Read off the live handle | `nvm3: ... 40960 B at 0x08174000`, the boot line's own last two fields |

A `.gbl` upload therefore leaves the instance intact, which was measured rather
than assumed: the device stayed commissioned across three reflashes in this
session, including the two that swapped the scratch probe in and out.

Occupancy, all from the boot line, in the order the session produced them:

| Moment | Objects | `availableMemory` | Erase count |
|---|---|---|---|
| Factory-fresh, one endpoint stored | 13 | 7 252 B | 1 |
| Factory-fresh, two endpoints stored | 13 | 7 052 B | 1 |
| **After commissioning one fabric** | **37** | **3 432 B** | 1 |
| Immediately after `AT+MTFRESET` | 11 | 2 668 B | 1 |
| After the Phase 1 reruns and the link burst | 37 | 480 B | 3 |
| A few boots later | 37 | 5 392 B | 3 |
| At the end of the session | 37 | 1 328 B | 3 |

Commissioning costs **24 NVM3 objects and about 3.6 KB** of the available
figure. Everything else in that table is one fact, and it is the same fact the
nRF arm's ZMS watch item records in a different spelling: **NVM3 is
log-structured, so `availableMemory` is not `size - used`.** A deleted object
keeps its flash until a repack collects it, which is why the row straight after
a factory reset has 26 fewer objects and LESS memory available than the row
before it, and why the figure climbs again without anything being deleted (the
erase count going 1 to 3 is the repack running twice). The 480 B row is a
pre-repack trough, not an exhaustion, and the 5 392 B row three boots later is
the same instance after collection.

Against the nRF arm's 30 476 of 32 768 B raw ZMS occupancy after a day of
commissioning churn: this instance is 40 960 B, and the comparison is not
like-for-like (that figure is raw non-erased bytes, this one is usable bytes
before a forced repack). What both say is the same thing: the settings region
is the part of this product whose sizing needs a soak test, and it stays a
watch item.

#### Harness Phase 0 and Phase 1, commissioned

Run twice on this image while commissioned as `0x4902`, with the rig's standard
composition (a single on/off light), which is what Phase 1 stages for itself.

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 1 \
      --baseline platform/silabs/core-phase1.json
  ===== RESULT: 292 passed, 4 failed =====
```

**292/4, row for row identical to task 6's uncommissioned run.** The result
file `platform/silabs/core-phase1.json` is rewritten against this image; the
only differences from task 6's are the header's `fw_repo_head` (`a9d6ae6...`),
its timestamp, and `net`, which moves from `+MTNET:THREAD,0,0,0` to
`+MTNET:THREAD,1,0,0` because Thread is enabled once the device has a fabric.
No verdict changed. Being on a fabric costs the AT contract nothing, which is
the thing this run existed to find out.

The four parked rows are task 6's four, unchanged and with the same owners
(catalogue batches 7a and 7b, the EVSE round): each stages a composition whose
device types this build's registry has no cluster set for.

#### Sustained traffic: the RX ring overflow warning has now fired

Round 1 left an open item saying the warning was observable but had never
fired, and that no run had pushed the link hard enough for its absence to be
evidence. Both halves of that were tested here, with
`platform/silabs/fw/link_burst.py`, which is committed beside `fw/flash.py` so
the measurement has a command like every other figure on this page. It is a
measurement tool, not a test: no pass criterion, exit 0 whatever it observes.

**A Phase 1 run while commissioned does not fire it**, and the reason is
structural rather than lucky: the harness is synchronous, so it never has more
than one command line in flight and the 1 024-byte ring never holds more than
one `MT_AT_LINE_MAX` line. 292 rows, nine module boots, 27 885 bytes of console
captured, zero warnings. That is still not evidence of headroom.

**A back-to-back burst fires it, the first time in this port's life.** 300
`AT+MTATTR=1,6,0` lines, 5 100 bytes, written with no waiting for terminal
responses:

```bash
export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
python3 platform/silabs/fw/link_burst.py --port "$MT_PORT" --lines 300
```

```
sent      300 line(s) of 17 B = 5100 B, written in 0.42 s (back to back)
drained   4.2 s (stopped after 4.0 s of silence; cap 120 s)
back      1334 B, 128 complete line(s)
          OK 59, ERROR 5, +MTERR 5, +MTATTR 59, unrecognised 0
terminals 64 of 300 command line(s) answered
```

and on the console, eleven warnings summing to **1 581 bytes dropped**:

```
W link: rx ring overflow, dropped 5 byte(s)
W link: rx ring overflow, dropped 45 byte(s)
... eight more ...
W link: rx ring overflow, dropped 584 byte(s)
```

Those counts are trustworthy in one specific way: `s_rx_ring.dropped` is
incremented per byte and never reset, and `hearth_link_read()` reports the
delta since the last read (`port/hearth_port_sl.c`), so the warnings sum to the
true total the ring rejected. The same burst run twice more gave 23 warnings and
1 261 bytes, and (with a controller toggling the light concurrently over
Thread, `--toggle-node 0x4902`) 12 warnings and 1 828 bytes: the shape repeats,
the exact figures do not, which is what a contended measurement looks like.

##### What the terminal count is NOT a measurement of

64 answered of 300 sent does **not** reconcile with 1 581 bytes dropped: 3 519
bytes reached the ring, which is 207 whole command lines, and only 64 were
answered. The gap is not in Hearth, and the paced control is what shows it.
Same tool, same device, same command, pacing the writes instead of bursting
them:

| Offered | Lines | Answered | Ring drops |
|---|---|---|---|
| 60 lines paced 2 ms apart (8 500 B/s) | 60 | **60 of 60** | none |
| 30 lines paced 5, 10 or 20 ms apart | 30 | **30 of 30** each time | none |
| 60 lines paced 1 ms apart (17 000 B/s) | 60 | 27 of 60 | **none** |
| 30 lines back to back (25 000 B/s) | 30 | 22 of 30 | **none** |
| 120 lines back to back | 120 | 43 of 120 | none |
| 300 lines back to back | 300 | 64 of 300 | 11 warnings, 1 581 B |

The break is between 1 ms and 2 ms per 17-byte line, and it is exactly where
arithmetic says it should be: **2 ms per line is 8 500 B/s, under the wire's
11 520 B/s at 115200; 1 ms per line is 17 000 B/s, over it.** Below the wire
rate the module answers every single line. Above it the excess cannot be
carried, and the three rows that lose lines while the module's own drop counter
reads zero (1 ms paced, 30 back to back, 120 back to back) are the proof of
where it goes: the loss is in the host-side CPico USB-CDC-to-UART bridge, which has no
way to push back on a host writing into a USB endpoint faster than 115200 can
drain it, and whose losses the module can neither see nor count. Only the
largest burst overruns the module's own ring on top of that.

So the honest split is: **the answered count measures the bench's write path,
and the warning measures Hearth.** What this section closes is the round 1 open
item, and only that: the ring drops bytes under real overrun, says how many on
the console, and the link recovers at the next complete line. The device was
healthy after every row of that table, checked after the contended 300-line
burst (`+MTSTATE:2,1`, `+MTFABRICS:1`, the composition intact, chip-tool still
able to toggle it, `AT+MTTHREAD?` settling to `ROUTER`) and again after the
whole sweep.

This is the contract working, not a defect. This carrier wires no RTS/CTS, so a
host that writes faster than the link can carry has nothing to throttle it but
the `OK`/`ERROR` handshake, and a host that honours that handshake never
reaches any of these rows. One accounting hole found while chasing the figures
is recorded under "What round 2 leaves open": the EUSART's own `RXOF` interrupt
is neither enabled nor checked, so a peripheral-level overrun would lose bytes
that `s_rx_ring.dropped` cannot count.

#### Harness Phase 2: not run, and the gate is not this image's

**Closed 2026-09-21**, all three of the obstacles this subsection names: the
gate takes otbr-agent's D-Bus route now, the SWD reset takes this carrier's
openocd config, and the `+MTEVT` assertions have events to assert against. The
run and its 98/2/1 are under "Harness Phase 2" above. What follows is round 2's
record of why it could not run, kept because it is the measurement the three
fixes were written against.

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 2
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ABORT: otbr-agent is not answering (ot-ctl state failed): start it per the
  T4 runbook (graph F36: run otbr-agent directly, D-Bus policy required)
```

**Zero rows ran** (graph F503). The abort is `phase2_gate`'s own precondition
(`test/mt_regression.py:6198`), which calls `otctl(["state"], args.ot_ctl)`.
The binary is not a PATH lookup and the gate is not a missing-tool problem:
`DEFAULT_OTCTL` (`test/mt_regression.py:5200`) is an absolute path into the
ot-br-posix build tree, that file exists and is executable, and running it by
hand shows exactly what the gate hits:

```
$ /mnt/.../ot-br-posix/build/otbr/third_party/openthread/repo/src/posix/ot-ctl state
connect session failed: Permission denied          exit 1
$ ls -l /run/openthread-wpan0.sock
srwxr-xr-x 1 root root 0 Sep 16 09:39 /run/openthread-wpan0.sock
$ systemctl is-active otbr-agent
active
```

**It is the socket** (graph F474, round 1's finding, which is why the dataset
read goes through D-Bus in the first place), not the tool and not the daemon:
otbr-agent is healthy and served the dataset over D-Bus for both commissionings
in this same session. The fix is to give the gate the same D-Bus route the
dataset already takes; `--dataset` / `MT_DATASET` covers the dataset half
already, and the liveness check has no override.

So this is a harness-versus-bench mismatch, not a firmware failure and not a
row to classify. It is a **Thread-arm allowance owned by the qualification
round**, recorded under "What round 2 leaves open" and in `TESTING.md`. Two
facts belong with it, both in that `TESTING.md` note:

- Phase 2's other precondition is `openocd`, because its 2.8 warm reboot resets
  an **RP2350** over SWD. That is the C6 carrier's contract, not this module's.
- Phase 2 asserts `+MTEVT` in eight of its steps (and Phase 3 in one more), and
  no non-C6 arm raises any event at all (graph B502). Those rows will fail for
  that one reason even once the gate is fixed. The exact steps and codes are
  listed in the `TESTING.md` note, derived from the harness source rather than
  from memory.

### Round 2 task 6: the attribute bridge and the `+MTATTR` URC

The current image, 2026-09-18, built from the committed tree at `cca6e0c` in a
fresh `~/silabs/work/hearth-matter-t6r` by the "Building" recipe above, flashed
with `fw/flash.py` over XMODEM. The AT link is the CPico carrier's CDC at
`/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00` with DTR and RTS
cleared before the open, the console is the Debug Probe's second CDC with DTR
asserted, and `otbr-agent` was not touched. Nothing is commissioned; that is
task 7's.

```
$ git status --porcelain      (clean)
$ slc generate -d ~/silabs/work/hearth-matter-t6r --sdk-package-path "$SISDK_ROOT" \
      --sdk-package-path "$MATTER_EXT_ROOT" -p platform/silabs/hearth.slcp \
      --with MGM240PA32VNA --generator-timeout=180 -o makefile
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t6r \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t6r/build.log
$ grep -ci warning   ~/silabs/work/hearth-matter-t6r/build.log    0
$ grep -c deprecated ~/silabs/work/hearth-matter-t6r/build.log    0
```

Sizes, with provenance (`arm-none-eabi-size` on
`~/silabs/work/hearth-matter-t6r/build/debug/hearth.out`, 2026-09-18, against
task 5's `~/silabs/work/hearth-matter-t5` figures):

| | Task 6 | Task 5 | Delta |
|---|---|---|---|
| `text` | 844 956 | 839 528 | +5 428 |
| `data` | 3 368 | 3 368 | 0 |
| `bss` (size's, includes the heap section) | 258 324 | 258 324 | 0 |
| `.text` (`-A`) | 844 120 | 838 692 | +5 428 |
| `.bss` (`-A`) | 117 720 | 117 688 | +32 |
| `.memory_manager_heap` | 135 992 | 136 024 | -32 |

The `+5 428 B` of `text` splits three ways and each part was measured
separately, because they are separate decisions: **3 272 B is the tiny printf
component** (measured as its own build of the same source, 844 288 with it
against 841 016 with newlib-nano), **396 B is ruling F500's carve-out** (the
table, the predicate, the live reader and the two dispatch arms: 844 956 against
the pre-ruling build's 844 560) and the remaining **1 760 B is the attribute
bridge itself**. The 32 B of `.bss` comes out of the heap section exactly, which
is the linker's leftover, so the two RAM rows reconcile; the carve-out's table
is `const` and lives in `text`.

`hearth.bin` 848 340 B (**55.08 %** of the 1 540 096 B application region, task
5: 842 912 B / 54.73 %), `md5sum 374a5cc6342a97e2f0fec1a9d94bb9dd`.
`hearth.gbl` 848 412 B, `md5sum 33eb0dbcb6a8f4eb6ab4fb4a64703dd8`, sent by
`fw/flash.py` as 6 629 blocks with 0 retransmits.

**The md5 identifies this one build and is not reproducible**, for the reason
task 5 recorded and proved with a `cmp`: OpenThread's version banner is built
from `__DATE__ __TIME__`, so two builds of the same tree differ in the six bytes
of the time-of-day digits at that string and nowhere else. A difference anywhere
but those six bytes would be a real difference.

#### Free heap at `+MTREADY`, task 6

**95 336 B** (`sl_memory_get_free_heap_size()`, logged by the boot task, with
the single-light rig composition rebuilt), out of the 135 992 B
`.memory_manager_heap`. Task 5's was 95 368 out of 136 024: the pool shrank by
the 32 B the new statics took and the free figure moved by exactly the same 32.
The bridge itself allocates nothing at run time, and the figure is unchanged by
ruling F500's carve-out, which is `const` data and four accessor calls.

#### The bench proofs, task 6

Every transcript below is from the flashed `374a5cc6` image. The driver prints
every line the device sends until `OK` or `ERROR`, so a `+MTATTR` URC that
arrives between a write and its `OK` is visible rather than absorbed;
`+MTERR:n` is always followed by `ERROR` on the wire and the collector returns
at the `ERROR`. Opening the CDC pulses the module's reset, so each batch starts
with a bare `AT` that drains the `+MTREADY` this produces.

The composition for the first two batches is task 5's light and temperature
sensor, endpoints 1 and 2.

##### The read and write legs, and the error division

```
AT+MTEP?                  -> +MTEP:0,1,0x0100 / +MTEP:1,2,0x0302 / OK
AT+MTATTR=1,6,0           -> +MTATTR:1,6,0,0 / OK       the light is off
AT+MTATTR=1,6,0,1         -> +MTATTR:1,6,0,1 / OK       the URC, then OK
AT+MTATTR=1,6,0           -> +MTATTR:1,6,0,1 / OK
AT+MTATTR=0,40,2          -> +MTATTR:0,40,2,65521 / OK  the root VendorID, F500
AT+MTATTR=2,1026,0,2222   -> +MTATTR:2,1026,0,2222 / OK
AT+MTATTR=2,1026,0        -> +MTATTR:2,1026,0,2222 / OK
AT+MTATTR=1,6,0,-1        -> +MTERR:1 / ERROR           minus on an unsigned
AT+MTATTR=1,0xFFFF,0      -> +MTERR:3 / ERROR           no such cluster on ep 1
AT+MTATTR=1,6,0xFFFF      -> +MTERR:4 / ERROR           no such attribute
```

All eight rows the task brief asked for. The root VendorID needed ruling F500's
carve-out, below; it answered a bare `ERROR` before that.

##### Ruling F500: the root node's identity, and the limit around it

```
AT+MTATTR=0,40,2          -> +MTATTR:0,40,2,65521 / OK   VendorID  0xFFF1
AT+MTATTR=0,40,4          -> +MTATTR:0,40,4,32784 / OK   ProductID 0x8010
AT+MTATTR=0,40,7          -> +MTATTR:0,40,7,1 / OK       HardwareVersion
AT+MTATTR=0,40,9          -> +MTATTR:0,40,9,1 / OK       SoftwareVersion
AT+MTATTR=0,0x0028,0x0002 -> +MTATTR:0,40,2,65521 / OK   hex form, same answer

AT+MTATTR=0,40,2,1        -> +MTERR:11 / ERROR           READONLY, now reachable
AT+MTATTR=0,40,4,1        -> +MTERR:11 / ERROR
AT+MTATTR=0,40,7,2        -> +MTERR:11 / ERROR
AT+MTATTR=0,40,9,2        -> +MTERR:11 / ERROR

AT+MTATTR=0,40,0          -> ERROR      DataModelRevision: NOT carved out
AT+MTATTR=0,49,0          -> ERROR      General Commissioning Breadcrumb: nor is it
AT+MTATTR=0,40,5          -> +MTERR:5 / ERROR   NodeLabel, a CHAR_STRING
AT+MTATTR=0,40,1          -> +MTERR:5 / ERROR   VendorName, the same
```

The last four rows are the limit, on the wire: the carve-out serves four named
attributes, every other code-driven integer on a fixed endpoint still answers a
bare `ERROR`, and every non-integer still answers `+MTERR:5` on type before
ember is reached. The bare `ERROR` carries its console line every time:

```
E matter: attr read ep 0 cluster 0x0028 attr 0x0000: ember status 134 (a fixed
          endpoint's code-driven cluster is not reachable through the ember path)
```

134 is `0x86`, `Status::UnsupportedAttribute`, from this port's own external
attribute read callback.

The dynamic endpoint is untouched by the carve-out: `AT+MTATTR=1,6,0` reads
`0`, a write to `1` echoes `+MTATTR:1,6,0,1` and answers `OK`, and a write back
to `0` echoes again, all in the same batch.

##### The as-built URC rules, and the seed values

```
AT+MTATTR=1,6,0,1         -> +MTATTR:1,6,0,1 / OK    mode defaults to 1: echoes
AT+MTATTR=1,6,0,0,1       -> +MTATTR:1,6,0,0 / OK    mode 1 explicit: echoes
AT+MTATTR=1,6,0,1,0       -> +MTATTR:1,6,0,1 / OK    MODE 0 ALSO ECHOES
AT+MTATTR=1,6,0,1,0       -> OK                      same value, mode 0: no URC
AT+MTATTR=1,6,0,1         -> OK                      same value, mode 1: no URC
AT+MTATTR=1,6,0,0,0       -> +MTATTR:1,6,0,0 / OK    a real change again: echoes
AT+MTATTR=2,1026,0        -> +MTERR:5 / ERROR        seeded null after the reset
AT+MTATTR=2,1026,0,2500   -> +MTATTR:2,1026,0,2500 / OK   out of null: echoes
AT+MTATTR=2,1026,0,-32768 -> OK                      INTO null: NO URC
AT+MTATTR=2,1026,0        -> +MTERR:5 / ERROR        and it reads back null
AT+MTATTR=0,40,5          -> +MTERR:5 / ERROR        NodeLabel is a CHAR_STRING
AT+MTATTR=1,6,0xFFFC      -> +MTATTR:1,6,65532,1 / OK
AT+MTATTR=1,6,0xFFFC,4294967296  -> +MTERR:1 / ERROR      the u32 width gate
AT+MTATTR=1,6,0,92233720368547758080 -> +MTERR:1 / ERROR  the 64-bit parse gate
AT+MTATTR=1,6,0xFFFD      -> +MTATTR:1,6,65533,6 / OK
AT+MTATTR=2,1026,0xFFFD   -> +MTATTR:2,1026,65533,4 / OK
AT+MTATTR=1,3,0xFFFD      -> +MTATTR:1,3,65533,6 / OK
```

That covers every as-built rule this section owes: **both notify modes echo**,
a **same-value write is `OK` and raises no URC in either mode**, a transition
**into** null raises no URC while a transition **out of** null does, a null
value reads as `+MTERR:5`, a non-integer type reads as `+MTERR:5`, and the width
gates refuse rather than truncate.

The last four rows close an item task 5 left open: its seed VALUES were "not
observable until Task 6's `AT+MTATTR` lands". They are now, and they are the
table's: OnOff FeatureMap `1` (Lighting), OnOff ClusterRevision `6`,
TemperatureMeasurement ClusterRevision `4`, Identify ClusterRevision `6`, and
TemperatureMeasurement MeasuredValue seeded null.

##### Step 1b: a live endpoint answers the cluster code, a dead one the endpoint code

```
AT+MTEP?                              -> +MTEP:0,1,0x0100 / +MTEP:1,2,0x0302 / OK
AT+MTALARM=1,0,0                      -> +MTERR:3   the light carries no alarm cluster
AT+MTALARM=2,1,1                      -> +MTERR:3   nor does the sensor
AT+MTMEAS=1,153,0,1                   -> +MTERR:3
AT+MTMEAS=1,152,3,-5000000000         -> +MTERR:3
AT+MTDEMCAP=1,1,0                     -> +MTERR:3
AT+MTROWGET=1,1                       -> +MTERR:4   the row family's own code
AT+MTROWGET=1,1,0                     -> +MTERR:4
AT+MTROWGET=2,1                       -> +MTERR:4
AT+MTMETERID=1,0,"A","B","C",100,200, -> +MTERR:4   the meter family's own code

AT+MTALARM=99,1,1                     -> +MTERR:2   THE CONTROL: 99 is not live
AT+MTMEAS=99,153,0,1                  -> +MTERR:2
AT+MTDEMCAP=99,1,0                    -> +MTERR:2
AT+MTROWGET=99,1                      -> +MTERR:2
AT+MTMETERID=99,0,"A","B","C",100,,   -> +MTERR:2
```

The dead-endpoint control rows are the half that makes the live rows mean
something: the same five commands still divide the two cases, and they divide
them at the endpoint table rather than by refusing everything.

#### Harness Phase 0 and Phase 1, round 2 task 6

Run with the rig's standard composition, a single on/off light on endpoint 1,
staged and applied before the run. The device was left in that state
afterwards, verified (`AT+MTEP?` -> `+MTEP:0,1,0x0100`, `AT+MTATTR=1,6,0` ->
`+MTATTR:1,6,0,0`, `AT+MTFABRICS?` -> `+MTFABRICS:0`).

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ===== RESULT: 0 passed, 0 failed =====      (green: the gate is the phase)

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 1 \
      --baseline platform/silabs/core-phase1.json
  ===== RESULT: 292 passed, 4 failed =====
```

**292/4 against task 5's 267/29.** The full result set is committed as
`platform/silabs/core-phase1.json`, beside round 1's
`platform/silabs/skeleton-phase1.json`. Twenty-five rows moved, all in the same
direction: all eleven `MTATTR` rows (the root VendorID read with ruling F500's
carve-out) and thirteen of the eighteen step 1b rows, plus the `MTATTR hex
equals decimal` row.

The four that remain, each with its cause, and they are one fact four times:

| Row | Cause |
|---|---|
| `MTMEAS staged variant-1 water heater` | Stages `0x0100, 0x050F,1` and needs the water heater to rebuild. Catalogue batch 7b. |
| `MTDEMCAP/MTMEAS staged variant-1 solar, battery and DEM` | Stages `0x0100, 0x0017,1, 0x0018,1, 0x050D,1`. Catalogue batch 7a. |
| `MTROWAPPLY count-0 ... on a real EVSE endpoint` | Stages `0x0100, 0x050C,1, 0x0511`. The EVSE round and batch 7a's meter. |
| `Utility meter pool exhaustion (MT_METER_MAX=2)` | Stages a light and three meters (`0x0511`) and wants the two-meter prefix back. Batch 7a's meter, plus a meter Instance pool this image has none of. |

Each stages a composition whose device types this build's registry has no
cluster set for, so the rebuild stops at the first of them and `AT+MTEP?`
answers the bare light. Each one's diagnosis line says exactly that
("composition readback wrong: ['+MTEP:0,1,0x0100']"). They were not reachable
by this task and they are not defects: they close when their catalogue batch
lands.

### Round 2 task 5: dynamic endpoints from the stored composition

Built 2026-09-18 from the committed tree at `97c6a31` in
`~/silabs/work/hearth-matter-t5` by the "Building" recipe above (a clean `slc
generate` into an empty directory), flashed with `fw/flash.py` and run on the
MGM240PA32VNA3 on the iLabs RP2350 carrier. `hearth.bin`
`md5sum 96e64e3fc7d93f9bd3b19bce8ffba21b`; that file is still in the build
directory and is the image every figure and every bench transcript below was
taken from.

This image is task 4's with `port/mt_devtypes_sl.cpp` and
`port/mt_dyn_store.h` in it, the live endpoint table appended to
`port/mt_matter_sl.cpp` and `rebuild_composition()` filled in `src/main.cpp`
("Port sections" above). It builds two device types out of the catalogue's 52
and knows all 52 for the gate predicates.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t5 \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t5/build.log
$ grep -c deprecated ~/silabs/work/hearth-matter-t5/build.log
0
$ grep -ci warning ~/silabs/work/hearth-matter-t5/build.log
0
```

**That log is a reproduction run, and the honest provenance is worth four
lines.** The first build of this task was captured without the documented
`tee`, so the two greps above had no surviving log to stand on. The tree was
rebuilt from `38802da` (the comment-only citation fix, no source change since
`97c6a31`) with a fresh `slc generate` and the recipe exactly as written; the
log is `~/silabs/work/hearth-matter-t5/build.log`, 113 906 B, and both greps
answer 0 against it. Every `arm-none-eabi-size` figure below reproduced to the
byte.

**The image is NOT byte-reproducible, and that is a property of the SDK rather
than of this project.** The rebuild produced `md5sum
8b596ebb4325cc84e5b384967140cb08`, kept beside the flashed image as
`hearth.rebuild-38802da.bin`, the same 842 912 bytes. `cmp -l` puts the whole
difference at **six bytes**, all of them inside OpenThread's version banner at
offset `0xB42E4`:

```
SL-OPENTHREAD/3.0.2.0_GitHub-61e43cffb; EFR32; Sep 18 2026 19:37:49   (flashed)
SL-OPENTHREAD/3.0.2.0_GitHub-61e43cffb; EFR32; Sep 18 2026 20:08:20   (rebuild)
```

That string is `__DATE__ __TIME__` compiled into the OpenThread stack, so an
`md5sum` recorded here identifies ONE build and can never be reproduced by a
later one; only the six timestamp bytes may differ, and a difference anywhere
else would be a real difference. The same six-byte result is also the evidence
that the comment-only commit between the two trees moved no code byte.

`arm-none-eabi-size ~/silabs/work/hearth-matter-t5/build/debug/hearth.out`,
with task 4's figures beside it:

| | Task 5, bytes | Task 4, bytes | Delta |
|---|---|---|---|
| `text` | 839 528 | 828 260 | +11 268 |
| `data` | 3 368 | 3 352 | +16 |
| `bss` (size's, includes the heap section) | 258 324 | 258 340 | -16 |

`arm-none-eabi-size -A` on the same file: `.text` 838 692 (task 4: 827 424,
**+11 268**), `.data` 3 368 (+16), `.bss` 117 688 (task 4: 114 104, **+3 584**),
`.memory_manager_heap` 136 024 (task 4: 139 624, **-3 600**), `.stack` 4 608,
`.nvm` 40 960, `text_application_ram` 448, `.vectors` 368,
`.bootloader_reset_section` 4, `.ARM.exidx` 8, `.copy.table` 12.

Those three RAM rows are one story and it reconciles: the heap section is
whatever the linker has left over, so the 3 584 B `.bss` gained and the 16 B
`.data` gained come straight out of it (3 584 + 16 = 3 600). The `.bss` is
**3 072 B of endpoint arena** (`HEARTH_EP_ARENA_BYTES`) plus about 512 B of
tables: `s_dyn[16]` (the dynamic endpoint headers), the live endpoint table's
five parallel arrays and the two counters. The `.data` 16 B is the
`hearth_arena` object itself, which has an initialised base pointer. The flash
is the 52-row registry, the seed table, the two device types' const metadata in
`.rodata`, the create path and the ember callbacks.

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus  842912 Sep 18 19:39 hearth.bin
-rw-rw-r-- 1 pontus pontus  842984 Sep 18 19:39 hearth.gbl
-rwxrwxr-x 1 pontus pontus 2528792 Sep 18 19:39 hearth.s37
```

The application image on flash is **842 912 B**, 54.73 % of the 1 540 096 B
application region, against task 4's 831 628 B and 54.00 %. `hearth.gbl` is
**842 984 B**, which `fw/flash.py` sent as 6 586 blocks in 93.6 s with no
retransmits (task 4: 6 498 blocks in 92.2 s).

#### Free heap at `+MTREADY`, task 5

**95 368 B free**, logged by `sl_memory_get_free_heap_size()` immediately after
`mt_at_start()` returns, out of the 136 024 B `.memory_manager_heap`. Task 4's
figure was 98 968 B out of 139 624 B: the heap section shrank by exactly the
3 600 B the new statics took, and the free figure moved by exactly the same
3 600. **The composition itself costs the heap nothing**, which is the whole
point of the arena: the figure is identical with 0, 1 and 2 endpoints rebuilt,
because every endpoint block comes out of `.bss` and not out of this pool.

#### The endpoint arena's occupancy

`rebuild_composition()` logs it once per boot, beside the line it explains.
Three of the boots below are in "The bench proofs" section; the numbers are the
sizing table in `port/mt_devtypes_sl.cpp` exactly:

```
I boot: composition rebuilt: 0 endpoint(s)
I devtypes: endpoint arena: 0 of 3072 B handed out, 3072 B free, 0 of 16 serviceable endpoints live

I boot: composition rebuilt: 1 endpoint(s)
I devtypes: endpoint arena: 192 of 3072 B handed out, 2880 B free, 1 of 16 serviceable endpoints live

I boot: composition rebuilt: 2 endpoint(s)
I devtypes: endpoint arena: 352 of 3072 B handed out, 2720 B free, 2 of 16 serviceable endpoints live
```

192 is the on/off light (3 clusters, 11 attribute slots, 188 payload bytes
rounded up to 8) and 160 is the temperature sensor (3 clusters, 9 slots, 156
payload). Handed out plus free equals the arena's capacity on every line,
because the cost model is rounding alone.

### Endpoint capacity, round 2 task 5

| | |
|---|---|
| A host may DECLARE | 28 endpoints (`MT_COMP_MAX_ENDPOINTS`, the AT contract, both arms) |
| This build can SERVE | 16 endpoints (`kServiceableEndpoints`, `CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT`) |
| The arena holds | 16 on/off lights (3 072 B, exactly full) or 19 temperature sensors, were the header table deeper |
| Creatable device types | 2 of the catalogue's 52: 0x0100 and 0x0302 |
| Every other catalogue id | known to `AT+MTEP=` with its real `max_variant` and parenting rule; refused at the boot rebuild, which then keeps the prefix |

A composition longer than 16, or one naming a device type this build does not
construct, fails at that entry and **keeps the endpoints before it live with
their ids unchanged** (`AT_MT_SPEC.md` 501-506). It is stop-at-failure, not
roll-back and not skip-and-continue: skipping would renumber every later
endpoint and hand a commissioned controller a silently different data model.

#### The bench proofs, task 5

Captured 2026-09-18, both ports at once: the console on the Debug Probe's UART
CDC at 115200 with DTR asserted, the AT link on the carrier's CDC with DTR and
RTS cleared before the open. `otbr-agent` was not touched and nothing was
commissioned.

An empty composition, and a light plus a temperature sensor:

```
AT+MTEPCLEAR   -> OK
AT+MTEPAPPLY   -> OK, reboot
AT+MTEP?       -> OK                      (no rows)
   console: composition rebuilt: 0 endpoint(s)

AT+MTEPCLEAR   -> OK
AT+MTEP=256    -> OK                      (on/off light, 0x0100)
AT+MTEP=770    -> OK                      (temperature sensor, 0x0302)
AT+MTEPAPPLY   -> OK, reboot
AT+MTEP?       -> +MTEP:0,1,0x0100
                  +MTEP:1,2,0x0302        OK
   console: composition rebuilt: 2 endpoint(s)
```

The gate predicates, answered from the registry on the `AT+MTEP=` line with an
on/off light staged at index 0. `+MTERR:n` is followed by `ERROR` on the wire;
only the code is shown:

```
AT+MTEP=0x0071,0,0  -> +MTERR:1   cabinet under a light: not a fridge or an oven
AT+MTEP=0x0077      -> +MTERR:1   cook surface unparented: it REQUIRES a cooktop
AT+MTEP=0x0077,0,0  -> +MTERR:1   cook surface under a light
AT+MTEP=0x0071      -> OK         the positive control: unparented IS legal for a cabinet
AT+MTEP=0x0100,1    -> +MTERR:1   the light's max_variant is 0
AT+MTEP=0x050F,1    -> OK         an UNPORTED row's max_variant is real: 0x050F has two variants
AT+MTEP=0x0050      -> +MTERR:6   not a catalogue device type at all
```

An unported type is accepted at staging and refused at the rebuild, with the
prefix kept:

```
AT+MTEPCLEAR  -> OK
AT+MTEP=256   -> OK
AT+MTEP=0x000A -> OK              door lock: a real catalogue id, nRF batch 3
AT+MTEP=770   -> OK
AT+MTEPAPPLY  -> OK, reboot
AT+MTEP?      -> +MTEP:0,1,0x0100    OK    the prefix, not the declared three
```

```
I mt_comp_store: loaded composition: 3 endpoint(s)
E devtypes: devtype 0x000A is in this build's registry but has no cluster set: the
            catalogue batch that builds it has not been ported yet, so the
            composition naming it cannot be rebuilt
E boot: endpoint 1 (0x000A) failed, aborting rebuild
I boot: composition rebuilt: 1 endpoint(s)
```

`AT+MTFRESET` completes, which is what ruling B493's `return 0` in
`mt_matter_evse_targets_erase_all()` buys:

```
AT+MTEP?     -> +MTEP:0,1,0x0100   OK
AT+MTFRESET  -> OK, reboot
AT+MTEP?     -> OK                 (erased)
AT+MTFABRICS? -> +MTFABRICS:0      OK
   console: mt_comp_store: composition erased, device is unconfigured
            composition rebuilt: 0 endpoint(s)
```

#### Harness Phase 0 and Phase 1, round 2 task 5

Phase 0 green, and Phase 1 run with the rig's standard composition, a single
on/off light on endpoint 1, staged and applied before the run:

```
$ export MT_PORT=/dev/serial/by-id/usb-iLabs_CPico_2350_5203321CE65EDFA5-if00
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
  ===== RESULT: 0 passed, 0 failed =====
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 1
  ===== RESULT: 267 passed, 29 failed =====
```

**267/29, against task 4's 264/32**, and the three rows that moved are exactly
the three parent-gate rows, all of them now answered by the registry rather
than by an accept-all stub:

- `[AT-] MTEP=0x0071,0,0 under a light -> +MTERR:1`
- `[AT-] MTEP=0x0077 unparented -> +MTERR:1`
- `[AT-] MTEP=0x0077,0,0 under a light -> +MTERR:1`

Nothing else changed verdict in either direction.

**Some rows this task was expected to close did not close, and the reason is
one fact rather than eighteen.** The task brief predicted that `MTALARM`, the
`MTROW*`, `MTMETERID` and `MTMEAS` wrong-cluster rows would turn green once an
endpoint existed for them to land on, because each expects `+MTERR:3` or
`+MTERR:4` (cluster or attribute not found on an endpoint that DOES exist)
rather than `+MTERR:2` (endpoint not found). Endpoint 1 now does exist, and
those rows still answer `+MTERR:2`, because **their bridge functions do not
consult the data model at all**: `mt_matter_alarm_set()`,
`mt_matter_meas_set()`, `mt_matter_row_*()` and `mt_matter_meterid_set()` are
still `port/mt_matter_stub.c` bodies that return `MT_ATTR_ERR_ENDPOINT`
unconditionally, whatever the endpoint table says. Measured directly, with the
light live on endpoint 1:

```
AT+MTALARM=1,0,0          -> +MTERR:2   (wanted 3)
AT+MTROWGET=1,1           -> +MTERR:2   (wanted 4)
AT+MTMEAS=1,153,0,1       -> +MTERR:2   (wanted 3)
```

The 29 remaining failures therefore divide cleanly: **eleven are `MTATTR`
rows** waiting on Task 6's attribute bridge, and **eighteen belong to
upward-port sections that have not moved at all** (the alarm bridge, the
measurement bridge, the row-staging bridge, the meter identity bridge), each of
which needs its own transfer before it can tell "that endpoint has no such
cluster" from "there is no such endpoint". That is a plan finding rather than a
defect in this task: nothing on the device-type side can make a stub that never
looks at the data model answer a data-model question.

### Round 2 task 4: the AT surface answers from the stack

Built 2026-09-18 from the committed tree at `6bece87` in
`~/silabs/work/hearth-matter-t4` by the "Building" recipe above (a clean
`slc generate` into an empty directory), flashed with `fw/flash.py` and run on
the MGM240PA32VNA3 on the iLabs RP2350 carrier. `hearth.bin`
`md5sum 2cf2d7c67d558b1a5a0c2198769df996`.

This image is task 3's with `port/mt_matter_sl.cpp` in it: nine of the sixty-one
`mt_matter.h` entry points now answer from CHIP and OpenThread rather than from
a stub. Nothing else about the boot changed.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t4 \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t4/build.log
$ grep -c deprecated ~/silabs/work/hearth-matter-t4/build.log
0
$ grep -ci warning ~/silabs/work/hearth-matter-t4/build.log
0
```

`arm-none-eabi-size ~/silabs/work/hearth-matter-t4/build/debug/hearth.out`, with
task 3's figures beside it:

| | Task 4, bytes | Task 3, bytes | Delta |
|---|---|---|---|
| `text` | 828 260 | 824 676 | +3 584 |
| `data` | 3 352 | 3 352 | 0 |
| `bss` (size's, includes the heap section) | 258 340 | 258 340 | 0 |

`arm-none-eabi-size -A` on the same file: `.text` 827 424 (task 3: 823 840,
**+3 584**), `.data` 3 352, `.bss` 114 104, `.memory_manager_heap` 139 624,
`.stack` 4 608, `.nvm` 40 960, `text_application_ram` 448, `.vectors` 368,
`.bootloader_reset_section` 4, `.ARM.exidx` 8, `.copy.table` 12. **Every RAM row
is task 3's to the byte**; the whole cost of this task is 3 584 B of flash, and
it is the code the linker could previously discard: the commissioning window
manager's open path, the setup-payload QR and manual-code generators, the fabric
table count and the OpenThread role and dataset getters.

```
$ ls -l hearth.bin hearth.gbl hearth.s37
-rwxrwxr-x 1 pontus pontus  831628 Sep 18 18:48 hearth.bin
-rw-rw-r-- 1 pontus pontus  831700 Sep 18 18:48 hearth.gbl
-rwxrwxr-x 1 pontus pontus 2494928 Sep 18 18:48 hearth.s37
```

The application image on flash is **831 628 B**, 54.00 % of the 1 540 096 B
application region, against task 3's 828 044 B and 53.77 %. `hearth.gbl` is
**831 700 B**, which `fw/flash.py` sent as 6 498 blocks in 92.2 s with no
retransmits (task 3: 6 470 blocks in 91.8 s).

#### Free heap at `+MTREADY`, task 4

**98 968 B free**, logged by `sl_memory_get_free_heap_size()` immediately after
`mt_at_start()` returns, out of the 139 624 B `.memory_manager_heap`. That is
task 3's figure **unchanged to the byte**, which is the expected result: this
task added code, not allocations, and none of the nine functions runs before the
marker. Everything task 3's section says about why this figure is not the steady
state (the boot task logs it before the Bluetooth task is scheduled, and it
still counts the boot task's own 5 KiB stack) applies here word for word.

#### The AT surface, task 4

Captured 2026-09-18, both ports at once: the console on the Debug Probe's UART
CDC at 115200 with DTR asserted, the AT link on the carrier's CDC with DTR and
RTS cleared before the open, whose own reset is the boot being watched. The AT
port carried `+MTREADY` and nothing else before the first command.

```
AT               ->  OK
AT+MTSTATE?      ->  +MTSTATE:1,0            OK
AT+MTFABRICS?    ->  +MTFABRICS:0            OK
AT+MTCODES?      ->  +MTCODES:MT:SAGA442C00KA0648G00,34970112332   OK
AT+MTCODES?      ->  +MTCODES:MT:SAGA442C00KA0648G00,34970112332   OK
AT+MTNET?        ->  +MTNET:THREAD,0,0,0     OK
AT+MTTHREAD?     ->  +MTTHREAD:UNSPECIFIED,0,,,,,""                OK
AT+MTCOMMISSION  ->  OK
AT+MTSTATE?      ->  +MTSTATE:1,0            OK
AT+MTCOMMISSION=300 -> OK
```

Those onboarding codes are the SDK's default example credentials (passcode
20202021, discriminator 3840, the console says so on every boot), not a secret;
the same pair is on the air in the BLE service data task 3 recorded.

Three of those answers are worth reading twice, because two of them are not what
a first guess predicts:

- **`AT+MTSTATE?` is `1`, not `0`, on a factory-fresh device, and that is
  correct.** `Server::Init()` opens a basic commissioning window during boot
  (task 3's console capture shows `Updating services using commissioning mode
  1` before `+MTREADY`), so `IsCommissioningWindowOpen()` is true and
  `mt_matter_state()` answers `MT_STATE_COMMISSIONING`. `AT_MT_SPEC.md` 3.5 says
  as much in its own note, and the harness agrees: `t_state_fabrics_consistent`
  allows state 1 with a fabric count of 0 and only forbids state 2 with 0 and
  state 0 with a non-zero count.
- **`AT+MTNET?` reports `<enabled>` 0 before commissioning, and that is a known
  divergence between the arms rather than a settled answer.** This port takes
  the figure from `ConnectivityMgr().IsThreadEnabled()`, which CHIP defines as
  "the device role is not `DISABLED`"
  (`GenericThreadStackManagerImpl_OpenThread.hpp:244-254`); the Thread interface
  is not brought up until a dataset arrives, so a device with the OpenThread
  task running and no network answers 0. **The C6 answers 1 in the same state**,
  because it does not ask: `platform/esp32c6/main/main.cpp:990-992` assigns
  `*enabled = 1` unconditionally under `CHIP_DEVICE_CONFIG_ENABLE_THREAD` and
  only `<connected>` is read from the stack. `AT_MT_SPEC.md:1022` defines
  `<enabled>` as "1 when the transport is compiled in and started", which is
  readable either way: compiled in and its task started (the C6's reading), or
  the interface actually up (CHIP's `IsThreadEnabled()`, this port's). **The
  spec has to say which**, and until it does a host that reads `<enabled>` gets
  a different answer from a C6 and an MG24 in the same state. Routed as a spec
  clarification, graph **F494**; nothing is changed here on one port's say-so.

  Nothing fails today: the Phase 1 row is `MTNET? format`, whose regex is
  `\+MTNET:(WIFI|THREAD),[01],[01],[01]` (`test/mt_regression.py:6374-6375`), a
  shape assertion that accepts either value, and the skeleton's stub answered 0
  as well, so the row's verdict did not move.

  The nRF arm compiles the same generic CHIP code and so should answer as this
  port does, but **that is an inference from shared source, not a measurement**:
  no `AT+MTNET?` transcript from the nRF bench was read for this report.

  The trailing `0` is `<mismatch>`, which this image can only ever answer 0
  (Thread is its only transport).
- **`AT+MTCOMMISSION` on a device whose window is already open answers `OK` and
  really does reopen it.** `CommissioningWindowManager::OpenBasicCommissioning
  Window()` has no incorrect-state check; it restores the discriminator, resets
  the failed-attempt counter and restarts the window. The console proves the
  call reached CHIP rather than being a no-op: `Updating services using
  commissioning mode 1` and the `Advertise commission parameter` line appear
  again 2.4 s into the session, long after boot.

`AT+MTFRESET` is **not** exercised here, and cannot be on this image.
`mt_matter_factory_reset()` is transferred and correct, but `cmd_mtfreset()`
erases the EVSE charging targets before it calls it
(`core/mt/mt_at.c`, the "same reason, and the deliberate asymmetry" block), and
`mt_matter_evse_targets_erase_all()` is still a stub returning -1, so the
command answers `+MTERR:7` and never reaches the reset. That is a code reading,
not a bench observation: the command was deliberately not sent, because its
first half erases the stored composition and would leave the bench in a state
no later task asked for. The row is the EVSE section's to close.

#### Harness Phase 0 and Phase 1, task 4

```
$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico --phase 0
  [GATE] preflight ok: MGM240P Hearth, firmware 1.2.0
===== RESULT: 0 passed, 0 failed =====

$ python3 test/mt_regression.py --port "$MT_PORT" --bridge cpico \
      --transport THREAD --phase 1
===== RESULT: 264 passed, 32 failed =====
```

The run was compared against `platform/silabs/skeleton-phase1.json` **name by
name, not count by count**: 296 rows on both sides, no row present on one side
only, and **exactly three verdict differences, all FAIL to PASS**:

- `MTCODES? format`
- `MTCODES? stable across reads`
- `MTTHREAD? shape by image (+MTERR:8 on WiFi, decoded line on Thread)`

Nothing regressed. The baseline file is not rewritten: it is round 1's skeleton
record, and the 32 rows still failing are listed in "The 35 failing rows" below
minus those three.

Host gates the same day, on the same tree: `make -C test/host run` green (exit
0, zero `[FAIL]` lines; `check_decls.py` **61/61** with no missing and no
duplicated across `mt_matter_stub.c` + `mt_matter_sl.cpp`, plus 4/4, 24/24 and
1/1, and `check_slcp_sources.py` silent) and
`python3 test/test_mt_regression.py` ran 467 tests, OK.

### Round 2 task 3: the Matter stack started

Built 2026-09-18 from the committed tree at `489531e` in
`~/silabs/work/hearth-matter-t3r` by the "Building" recipe above (a clean
`slc generate` into an empty directory), flashed with `fw/flash.py` and run on
the MGM240PA32VNA3 on the iLabs RP2350 carrier. `hearth.bin`
`md5sum 0162cb7e0b7e8407323717465e793054`.

These figures were re-recorded after the review fix (`489531e`), which adds 116
bytes of `.text`; the tree at `21b0b8c` measured 824,560 / 827,928 and its
directory `~/silabs/work/hearth-matter-t3` is left beside this one.

This image initialises `chip::Server`, runs the CHIP event loop and advertises
over BLE. The upward port is still `port/mt_matter_stub.c`, so the AT surface
answers exactly as it did.

```
$ POST_BUILD_EXE=$(which commander) make all -C ~/silabs/work/hearth-matter-t3r \
      -f hearth.Makefile -j8 | tee ~/silabs/work/hearth-matter-t3r/build.log
$ grep -c deprecated ~/silabs/work/hearth-matter-t3r/build.log
0
$ grep -ci warning ~/silabs/work/hearth-matter-t3r/build.log
0
```

`arm-none-eabi-size ~/silabs/work/hearth-matter-t3r/build/debug/hearth.out`, with
task 2's figures beside it:

| | Task 3, bytes | Task 2, bytes | Delta |
|---|---|---|---|
| `text` | 824 676 | 728 200 | +96 476 |
| `data` | 3 352 | 3 116 | +236 |
| `bss` (size's, includes the heap section) | 258 340 | 258 576 | -236 |

`arm-none-eabi-size -A` on the same file:

| Section | Task 3, bytes | Task 2 | Where |
|---|---|---|---|
| `.text` | 823 840 | 727 364 | flash |
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
-rwxrwxr-x 1 pontus pontus  828044 Sep 18 18:28 hearth.bin
-rw-rw-r-- 1 pontus pontus  828116 Sep 18 18:28 hearth.gbl
-rwxrwxr-x 1 pontus pontus 2484176 Sep 18 18:28 hearth.s37
```

The application image on flash is **828 044 B**, 53.77 % of the 1 540 096 B
application region, against task 2's 731 332 B and 47.49 %. The stock Silicon
Labs `lighting-app` for this part was 1 025 140 B with the shell, the OTA
requestor, scenes, level and colour control; the nRF54L15 core round shipped at
753 691 B. This image is now about 74 KB above the nRF's, which is the first
figure in this round where it is, and it carries a catalogue endpoint's worth of
cluster servers that no composition serves yet.

`hearth.gbl` is **828 116 B**, which `fw/flash.py` sent as 6 470 blocks in
91.8 s with no retransmits (task 2: 5 715 blocks in 81.0 s).

#### Free heap at `+MTREADY`, task 3

**98 968 B free**, logged by `sl_memory_get_free_heap_size()` immediately after
`mt_at_start()` returns, out of the 139 624 B `.memory_manager_heap`, and
unchanged by the review fix. Task 2's figure at the same point was 104 784 B, so the running stack has taken about
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
AT in full: b'+MTREADY\r\n', 10 bytes        (first byte at 0.343 s)
```

**Nothing before it and nothing else**, with the whole Matter stack coming up in
between. The marker arrives at 0.343 s against task 2's 0.151 s; the extra
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
I chip: [DL] BLE Static Device Address DB:60:00:F8:AF:DB
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
[NEW] Device DB:60:00:F8:AF:DB Hearth

$ bluetoothctl info DB:60:00:F8:AF:DB
Device DB:60:00:F8:AF:DB (random)
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

**Thirty of them are closed.** Round 2 task 4 turned `MTCODES? format`,
`MTCODES? stable across reads` and `MTTHREAD? shape by image` green; round
2 task 5 turned the three parent-gate rows (`MTEP=0x0071,0,0`, `MTEP=0x0077`
unparented and `MTEP=0x0077,0,0`) green; and round 2 task 6 turned twenty-five
more green, every `MTATTR` row and every step 1b row that does not need a
device type this build cannot create. The current image
fails **4** of the rows below, each named with its cause under "Measured",
"Round 2 task 6". The
list is left as it was recorded, because it is round 1's checklist and the
baseline file it was captured in is not rewritten; each later section says which
rows it closed.

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

**This section is closed as of round 2 task 7.** It said the RX ring overflow
warning (`W link: rx ring overflow, dropped N byte(s)`) was observable but had
never appeared, and that no run had pushed the link hard enough for its absence
to be evidence. Task 7 pushed it deliberately with
`platform/silabs/fw/link_burst.py` and it fired: eleven to twenty-three
warnings per 300-line burst, 1 261 to 1 828 bytes dropped, and the link
recovering at the next complete line. See "Sustained traffic: the RX ring
overflow warning has now fired" under "Round 2 task 7", which also says what
that run's terminal count does NOT measure. The ring is 1 024 bytes against a
512-byte `MT_AT_LINE_MAX`, and this carrier wires no RTS/CTS, so the
`OK`/`ERROR` handshake is the only thing throttling a host.

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

## What round 2 leaves open

Round 2 is the Matter core: the stack started before `+MTREADY`, the
commissioning, network, Thread, composition and attribute families answering
from it, a stored composition rebuilt as dynamic endpoints, and two device
types built. Everything it did not finish is here, and nothing here is unowned.
Round 1's own table follows this one and is still live; the rows round 2 closed
are marked there.

### The catalogue

The registry carries all 52 rows and the gate predicates; this build constructs
fourteen of them (round 2's two milestone types plus catalogue batch 1's
twelve). The rest arrive in the nRF arm's own order, batch by batch, each
copying its sections out of `platform/nrf54l15/port/mt_devtypes_zephyr.cpp` at
the line ranges "Port sections" lists, adding its cluster components to
`hearth.slcp` and its clusters to the catalogue endpoint, and recording RAM in
the heap and the arenas.

| Batch | Contents | Brings with it |
|---|---|---|
| 1 | attribute-only, 14 types | nothing structural. **Built 2026-09-22**, twelve types: the registry's fourteen batch-1 ids include the two milestone types round 2 already built (0x0100, 0x0302). Proof file `core-batch1.json`; see "Catalogue batch 1" under "Measured" |
| 2 | server-interaction, 6 types | **the cluster-object arena** and the first delegate pool |
| 3 | command-verdict, 2 types | the `+MTCMD` verdict path |
| 4 | appliance, 7 types | OperationalState and Chime carve-out rows |
| 5 | standalone, 7 types | the Rvc mode rows |
| 7a | energy foundation, 6 types | the meter Instance pool, ElectricalPowerMeasurement, DEM |
| 7b | delegate-served pair, 2 types | WaterHeaterManagement |
| 8 | composed appliances, 7 types | the oven and refrigerator mode rows |
| EVSE | 1 type | EnergyEvse and its sixteen attributes |

**The four Phase 1 rows this round cannot pass belong to that list**, and they
are one fact four times: each stages a composition whose device types this
build's registry has no cluster set for, so the rebuild aborts at the first of
them and `AT+MTEP?` answers the bare light. They are not defects and they are
not a regression against the nRF arm.

| Parked row | Waits for |
|---|---|
| `MTMEAS staged variant-1 water heater` | batch 7b |
| `MTDEMCAP/MTMEAS staged variant-1 solar, battery and DEM` | batch 7a |
| `MTROWAPPLY count-0 ... on a real EVSE endpoint` | the EVSE round plus 7a's meter |
| `Utility meter pool exhaustion (MT_METER_MAX=2)` | 7a's meter, plus a meter Instance pool this image has none of |

### The wire surface

| Open item | Owner |
|---|---|
| **A dynamic endpoint's LevelControl server state was never initialised, so every `MoveToLevel` target clamped to 0: closed on this arm 2026-09-22** (graph **B525**) by catalogue batch 1's fix round, `ca05afa`. Found by the batch's own proof, six rows on 0x0101 and 0x010B; `MinLevel` and `MaxLevel` read 1 and 254 over the wire while the server's private `EmberAfLevelControlState` held 0 and 0, because `emberAfLevelControlClusterServerInitCallback()` is a per-cluster init function and `DECLARE_DYNAMIC_CLUSTER` hardcodes `.functions = NULL`. The port now defines the weak `emberAfLevelControlClusterInitCallback()` strongly and calls the server init from it, which is the hook the nRF arm already uses for DoorLock. Full account, the pre-fix transcript and the override under "Catalogue batch 1" | **closed here. The nRF half is open**: that arm declares the same tables through the same macro and has the same gap by construction, and nothing there has ever sent a level command |
| **`+MTEVT` is emitted on this port: closed 2026-09-21** (graph **B502**) by the `+MTEVT` parity round's task 1, in `platform/chip/mt_chip_events.cpp`, shared with the nRF arm and bench-proven here: the boot sequence, a full commissioning's order with the 0/4 pair, the role-cache suppression and `AT+MTFRESET`'s fabric bits, all under "Events (`+MTEVT`)" above. The round 2 row below it is what that fix was written against. **The nRF half closed the same day** (task 5): the Ophelia-IV carrier came back on the bench, ran the same shared file and recorded its own Phase 2 at the same **98 passed, 2 failed, 1 not applicable**, plus Phase 3's 3.5 with `+MTEVT:1` and `+MTEVT:3`. See `platform/nrf54l15/README.md`, "Events (`+MTEVT`)" | **closed on both Thread arms, 2026-09-21** |
| **Deferred minors from the parity round's final review: closed 2026-09-22 by catalogue batch 1's task 1**: `mt_chip_events_register()`'s registration order now puts `AddEventHandler()` last and sets `s_registered` right after it succeeds, so a retry after either idempotent call above it registers nothing twice; the `ScheduleWork()` failure path now clears `s_window_evt_sent`, so a lost `+MTEVT:4` no longer suppresses every later window's 0; the harness gate's role message now names its source, `ot-ctl state` or `busctl DeviceRole`; `busctl` absent from PATH is now a gate message rather than surfacing as "link lost", gated only on the real D-Bus route being in use; `--openocd-config` is checked for existence at the gate, before the destructive phase it guards; bit 11 carries the same unreachable-on-Thread annotation as bit 10 | **closed** |
| **A fixed endpoint's code-driven attributes are unreadable through `AT+MTATTR` except where ruling F500's carve-out serves them.** Four Basic Information integers are carved out and answer; every other code-driven integer on endpoint 0 answers a bare `ERROR` with a console line saying why. Closing it means a read path through the data model provider for endpoints this port did not create, which changes the answer for every fixed-endpoint attribute at once | a **later round by ruling**, not by omission. Whether `AT+MTATTR` should reach fixed endpoints at all is a wire-contract question for both arms and the ruling belongs in `AT_MT_SPEC.md` 3.8 |
| **`AT+MTNET?`'s enabled flag divides differently here** from the C6's (graph F494): this arm reports `+MTNET:THREAD,0,0,0` uncommissioned and `+MTNET:THREAD,1,0,0` once a fabric exists, and the harness's own baseline header records the change. It is a spec clarification, not a port fix | the **qualification round**, with `AT_MT_SPEC.md` |

### The image

| Open item | Owner |
|---|---|
| **The tiny printf faults on a NULL `%s`** rather than printing `(null)`, image-wide, for the SDK's, CHIP's and OpenThread's log call sites as much as Hearth's. `core/`, `port/` and `src/` were audited call site by call site and are clean, and the one place it could have happened is guarded deliberately (`core/mt/mt_at.c:1715-1719`); the SDK's, CHIP's and OpenThread's were not audited. The cheaper of the two fixes is to patch the component under `sdk-patches/` so `case 's'` prints `(null)` | the **qualification round**; both options and the line numbers are in the `.slcp` comment and "Where the tiny printf disagrees with newlib-nano" |
| **The cluster-object arena does not exist yet.** The mechanism does (`hearth_arena`, `port/mt_dyn_store.h`); the instance, its budget beside `HEARTH_EP_ARENA_BYTES` and the family pool arrive together or not at all | **catalogue batch 2** |
| **A fault before `hearth_console_init()` returns produces no output on any UART.** The console is the first thing `app_init_early()` does, so the window is small, but it covers `sl_clock_manager_init()` and the `device_init` steps | unowned by design: closing it means a pre-console sink (RTT is there, and is what the SDK's own early code uses). Listed so it is a known limit rather than a surprise |
| **A fault report is proven only as far as its log line.** The HardFault probe proved the report itself ("A log line from a fault handler does not wait for the mutex"), and `debugHardfault()` ends there. Three other hooks in `src/sdk/SoftwareFaultReports.cpp` do not end there: `vApplicationMallocFailedHook` (`:226`), `vApplicationStackOverflowHook` (`:246`, called from `vTaskSwitchContext()` inside PendSV) and `RAILCb_AssertFailed` (`:329`, the radio interrupt) all continue into `Silabs::OnSoftwareFaultEventHandler()` (`:75-99`), which is live because the generated `gen_config.h` defines `MATTER_DM_PLUGIN_SOFTWARE_DIAGNOSTICS_SERVER`, and which calls `vTaskGetInfo()`, `SystemLayer().ScheduleLambda()` and `osDelay(1000)` from handler mode. (`halInternalAssertFailed`, `:107-116`, does not: it logs and asserts.) The file is a verbatim SDK copy and is not edited here, so what follows the log line is the SDK's own contract, shared with every Silicon Labs sample, and nothing on this bench has exercised it | the **qualification round**, with two options: an `sdk-patches/` entry that makes the handler safe in handler mode, or dropping `matter_software_diagnostics` so the handler compiles to nothing |
| **`b9fea7f` is a commit that was made and then reverted** in task 6 (`fix: the image links the full newlib ...`, undone by `8879199` with the fault registers). It is left in history deliberately, because the reversal is evidence; a reader diffing the branch meets a change that is not in the result | the branch's final review, if it wants it squashed |
| **An md5 is not reproducible across `slc generate` runs** (F497): OpenThread's version banner is built from `__DATE__ __TIME__`. A relink inside one generated tree IS reproducible, which task 7 proved by accident. So an md5 identifies one generation, and a difference anywhere but the six time-of-day bytes is a real difference | settled, not open: stated wherever an md5 is quoted |

### The bench and the harness

| Open item | Owner |
|---|---|
| **Harness Phase 2 ran on this bench: closed 2026-09-21** (graph **F503**). All three obstacles are gone: the gate falls back to otbr-agent's D-Bus property when `ot-ctl` cannot open the root-owned socket (the `+MTEVT` round's task 3), the SWD reset takes this carrier's own openocd config (`--openocd-config platform/silabs/mg24-swd.cfg`, task 4), and the `+MTEVT` assertions have events (task 1). **98 passed, 2 failed, 1 not applicable**, every event row passing, result file `platform/silabs/core-phase2.json`; see "Harness Phase 2" | **closed here**. What is left of the row is the two failures, which are the attribute-persistence gap below, and Phase 3, which has never run on this arm |
| **A dynamic endpoint's attribute value does not survive a reboot**, warm or cold. Measured 2026-09-21 by harness rows 2.8 and 2.9 (`attribute value survived (B63 guard)`) and reproduced by hand: write `AT+MTATTR=1,6,0,1`, reset over SWD, read back `0`. Every attribute on every dynamic endpoint is `EXTERNAL_STORAGE`, so the value lives in the endpoint arena's RAM; CHIP's write path would persist a `NONVOLATILE` row, but nothing restores one, because `emAfLoadAttributeDefaults()` never runs for a dynamic endpoint and discards `EXTERNAL` rows where it does. The fix is a restore path in the port beside `rebuild_composition()`, before `+MTREADY`, and it needs a ruling on which attributes carry the spec's N quality plus a write-churn budget against the NVM3 row below. **The nRF arm declares the same tables through the same mechanism and carries no `NONVOLATILE` either**, so this is one gap for both Thread arms; **confirmed on the nRF 2026-09-21**, where the same two rows failed with the same reads (`platform/nrf54l15/core-phase2.json`) | the **qualification round**, with the nRF arm; the full account is under "Harness Phase 2" |
| **Harness Phase 2's cold boot needs this bench's hub recipe.** `uhubctl -l 3-1.3 -p 3 -a off` cuts the carrier's power without the kernel ever seeing the disconnect, so the device node goes stale rather than vanishing and step 2.9 times out; the matching `-a on` does not restore the port either. `uhubctl -l 3-1.3 -a cycle -d 5`, all ports, is what produces a real disconnect and what recovers a carrier left dark. A bench fact, recorded under "Harness Phase 2" | the bench; nothing in the harness or the port is wrong here |
| **NVM3's usable figure needs a soak** (graph **F503**). 40 960 B configured, 24 objects and about 3.6 KB consumed by one commissioning. `availableMemory` was observed as low as 480 B during round 2 task 7's write churn, before a repack returned it to 5 392 B, and **as low as 176 B on 2026-09-22, during catalogue batch 1's bench session** (a proof run, two Phase 2 runs and a measurement cycle in one day; erase count 5 to 8, so three repacks; the four boot figures are under "Catalogue batch 1"). **176 B is the number the soak has to size against**, not 480. Nothing failed and no write was refused, so the repack is the mechanism working, but the trough is unmeasured over a long run, exactly as the nRF arm's 32 KB ZMS row is | the **qualification round**'s soak, with the nRF row |
| **Phone commissioning over Thread has never been tried on this arm.** Both commissionings here are the CLI chip-tool on the border router's own host | the **qualification round** |
| **The EUSART's own RX overflow is neither enabled nor counted.** `EUSART0_RX_IRQHandler` drains the FIFO while `STATUS.RXFL` is set and never looks at `EUSART_IF_RXOF`, so a peripheral-level overrun would lose bytes that `s_rx_ring.dropped` cannot see and the console never reports. Not observed: task 7's burst accounting is fully explained by the ring's own counter plus a lossy host-side bridge. It is an accounting hole, not a known defect, and the cheap close is to enable `RXOF` and fold it into the same warning | whoever next touches `port/hearth_port_sl.c`; see "Sustained traffic" |
| **`fw/flash.py`'s `read_until()` and the harness's own stream loops do the same job in two places.** A round 1 review minor, still true, still costing nothing | whoever next touches either; it is a tidy-up, not a defect |

### The branch

| Open item | Owner |
|---|---|
| **This branch has not been rebased onto `dev/fota-firmware`**, which is where the user ruled it lands (graph **DE484**). Against the merge base `1842af3` that branch adds four declarations to `core/include/mt_matter.h` (`mt_matter_ota_set_mode`, `mt_matter_ota_block_acked`, `mt_matter_ota_staged`, `mt_matter_swver_set`), which `port/mt_matter_stub.c` has to answer or `check_decls.py` reads 61/65 and the image does not link; it adds `core/mt/mt_ota.c` to `core/sources.cmake`, which `hearth.slcp` has to list or `check_slcp_sources.py` fails; and it edits `test/host/Makefile`'s `TESTS` and its `run:` recipe, both of which this branch also edits (`run: all boundary silabs-stubs`), so that one is a textual conflict whose resolution is to keep both sides | **the user's call; the batch rounds continue on this branch by the 2026-09-18 ruling (DE484)**. None of the three is discovered at merge time: each has a check on this branch that names it. This round's own forecast: the branch carries seventeen pre-existing conflict hunks against `dev/fota-firmware` in four files (`platform/nrf54l15/CMakeLists.txt`, `test/host/Makefile`, `test/mt_regression.py`, `test/test_mt_regression.py`), none of them in this round's own hunks; once the rebase lands, `_transport_gate`'s two new door checks also gate `phase4_gate` (safe, both default through `getattr`); the twenty harness names `test/mt_catalogue_proof.py` imports all exist on that branch; and the ARCHITECTURE.md 8.x decision log still owes an entry for four MG24 rounds |

### Still owned elsewhere

Signing, secure boot and the SE debug lock stay **pre-ship** (round 1 spec
section 7, stage 2). Thread-arm baselines under `test/baselines/`,
ARCHITECTURE 8.21 and the host library's variant table stay with the
**qualification round**. Matter OTA stays never: the field update story is
host-driven serial flashing (`FIRMWARE_UPDATE_SPEC.md`). Raising the memory
tier above `kServiceableEndpoints` 16 is the **MG26 round**'s.

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

Round 2 Tasks 4 to 6 (2026-09-18) emptied the first row: the composition
rebuild, the dynamic endpoint machinery, the endpoint arena and the first two
device types are all in, and what is left of the catalogue is a table of its
own under "What round 2 leaves open". Round 2 Task 7 (2026-09-18) closed the RX
ring overflow row with a measurement, and the table's own remaining rows are
unchanged and still owned.

| Open item | Owner |
|---|---|
| ~~The upward port: the composition rebuild, the dynamic endpoint machinery, the arenas and the device-type catalogue~~ | **closed as the core, round 2 tasks 3 to 7**: the stack is started, the composition rebuilds as dynamic endpoints, the endpoint arena is live, the attribute bridge answers and two device types are built. What is left is the catalogue itself, which has its own table under "What round 2 leaves open" |
| Signing (ECDSA-P256 dev key under `keys/`, as the nRF port does), secure boot, and the SE debug lock after the one-time SWD install | **pre-ship** (design spec section 7, stage 2). The bootloader itself is built and installed already; it accepts an unsigned `.gbl` today |
| Thread-arm baselines under `test/baselines/`, ARCHITECTURE 8.21 and its decision-log rows, and the host library's `fw/README` variant table | the **qualification round** (design spec section 8, steps 6 and 7). The skeleton's Phase 1 record deliberately stays out of `test/baselines/`, which holds shipping baselines only |
| ~~The RX ring overflow warning is observable but has never fired~~ | **closed, round 2 task 7**: a 300-line back-to-back burst (`fw/link_burst.py`) fired it eleven times for 1 581 bytes, and the link recovered at the next complete line. See "Sustained traffic" under "Round 2 task 7" |
| The console is TX only by design, so the port has no console input path and no shell | settled, not open: it is board contract item 6 and a CRA posture (`CRA_COMPLIANCE.md` in the docs repository). Listed here so nobody reopens it as an omission |
| `CHIPProjectConfig.h` does not set `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` or its string, so BasicInformation reports the SDK default `1` / `"1.0"` while `AT+CGMR` answers `MT_FW_VERSION`, 1.2.0. Two version surfaces, one of them wrong. A second hand-maintained copy of the version would drift, so the fix is to derive it | the **qualification round**, which is what makes the two surfaces answerable together |
| Diagnostic Logs and Wi-Fi Network Diagnostics are disabled on THIS arm since task 3 and still enabled on the nRF arm's, so the two Thread ports' root nodes no longer answer the same | the **qualification round**, which owns the wire surface for both arms at once; see "Data model" |
| `src/sdk/SoftwareFaultReports.cpp` is a verbatim copy of an extension source, carried because slc cannot reference a file outside the project by a portable relative path. It goes stale silently on an SDK bump, exactly like the OpenThread override | whoever bumps the SDK; the diff command is in `src/sdk/README.md` |
| The Matter BLE advertisement carries the fixed name `Hearth` rather than the SDK's `<prefix><discriminator>` form, because `SetBLEDeviceName()` replaces that form rather than decorating it. Two units on one bench are indistinguishable by name, though on this build they would be anyway: the discriminator is the fixed test value | the **qualification round**; both macros and the reasoning are in `src/CHIPProjectConfig.h` |
| `config/sl_openthread_features_config.h` is a frozen 444-line copy of an SDK file with one value changed. It goes stale silently on an SDK bump | whoever bumps the SDK; the diff command is under "The OpenThread override" |
