# iLabs_Hearth Library Transport Catch-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the arduino-pico host library up to the firmware 0.2.0 AT
contract: the AT+MTTRANSPORT surface, the P2 mismatch flag and event 27,
and a truth-pass over the stale reset-semantics documentation.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-03-hearth-library-transport-catchup-design.md`
in the FIRMWARE repo): all new API lands on `HearthClass` (the extension
surface), never on `ArduinoMatter` (the upstream-parity surface). Event 27
routes through the existing link-event callback. Compat with single-stack
firmware is lazy: the wire's own `+MTERR:8` is the not-supported signal.

**Tech Stack:** C++ (Arduino), gcc host tests with MockStream, no hardware.

## Global Constraints

- **Work happens in the LIBRARY repo**: `/home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth`,
  branch `main`. It is Dropbox-synced: work on the files in place, never
  create symlinks into this tree. The plan and spec live in the firmware
  repo; do not commit library changes there.
- **No em dashes** anywhere: code, comments, docs, commit messages.
- **Parity surfaces stay byte-identical to upstream**: `src/Matter.h`, the
  `matterEvent_t` enum, and every `ArduinoMatter` method signature are
  untouched. New API is `HearthClass`-only (repo rule, `src/Hearth.h:6-10`).
- Library style: 2-space indent, `/* */` block comments, camelCase methods,
  `hearth` prefix on internal helpers. Match the surrounding code.
- Host tests: `make -C test/host run` from the library root, all binaries
  must pass. TDD per task: failing test first, then implementation.
- The firmware's `docs/AT_MT_SPEC.md` 3.12.2 (in the firmware repo at
  /mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/iLabs_AT_Hearth) is
  authoritative for the AT+MTTRANSPORT grammar. Wire names are upper-case
  exact: `WIFI`, `THREAD`.
- `lastError()` is the `+MTERR` code space (`src/Hearth.h:158`,
  `src/Hearth.cpp:190`): a `+MTERR:<n>` reply already lands there. Do not
  invent a parallel error enum.
- Commit messages explain why and end with exactly:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task 1: The fourth +MTNET field and transportMismatch()

**Files:**
- Modify: `src/Hearth.cpp` (HearthNetCtx ~line 543, hearthOnNetLine ~550,
  the stale comment ~540)
- Modify: `src/Hearth.h` (HearthClass public section, near linkUp())
- Test: `test/host/test_hearth.cpp` (additions)

**Interfaces:**
- Produces: `struct HearthNetCtx` gains `int mismatch;`;
  `bool HearthClass::transportMismatch();` declared in Hearth.h.
  Task 3's transport() does NOT depend on these.

- [ ] **Step 1: write the failing tests** (append to `test/host/test_hearth.cpp`,
matching its existing `static void test_*()` + `check(name, cond)` +
MockStream `expect(cmd, response)` conventions; copy the exact response
line-ending style from an existing expect() in this file):

```cpp
static void test_net_three_fields_still_parses() {
  /* Old firmware: no mismatch field. Must behave exactly as before. */
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTNET?", "+MTNET:WIFI,1,1\r\nOK\r\n");
  check("3-field isWiFiConnected", Matter.isWiFiConnected());
  ms.expect("AT+MTNET?", "+MTNET:WIFI,1,1\r\nOK\r\n");
  check("3-field no mismatch reported", !Hearth.transportMismatch());
  check("script drained", ms.scriptDrained());
}

static void test_net_four_fields_mismatch() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTNET?", "+MTNET:THREAD,1,0,1\r\nOK\r\n");
  check("4-field mismatch seen", Hearth.transportMismatch());
  ms.expect("AT+MTNET?", "+MTNET:THREAD,1,0,1\r\nOK\r\n");
  check("4-field connected still parses", !Matter.isThreadConnected());
  ms.expect("AT+MTNET?", "+MTNET:THREAD,1,1,0\r\nOK\r\n");
  check("4-field mismatch clear", !Hearth.transportMismatch());
  check("script drained", ms.scriptDrained());
}
```

Register both in this file's `main()` the way its other tests are.

- [ ] **Step 2: run to verify failure**

Run: `make -C test/host run`
Expected: compile FAILS (`transportMismatch` is not a member).

- [ ] **Step 3: implement.** In `src/Hearth.cpp`:

The context struct and doc comment (replacing the "fixed at build time"
comment at ~540-542):

```cpp
/* "+MTNET:<transport>,<enabled>,<connected>[,<mismatch>]" (AT_MT_SPEC.md
 * S3.12). One line per query: one transport is active per BOOT. On the
 * single-stack images that choice is fixed at build time; on the combined
 * image it follows the persisted AT+MTTRANSPORT setting. The fourth field
 * (0.2.0 firmware) is the transport-mismatch flag of S3.12.1; older
 * firmware sends three fields and the flag defaults to 0. */
struct HearthNetCtx {
  char transport[8];
  int enabled;
  int connected;
  int mismatch;
  bool got;
};
```

In `hearthOnNetLine()`, replace the final two lines of the parse
(`ctx->connected = ...; ctx->got = true;`) with:

```cpp
  ctx->connected = (int)strtol(end + 1, &end, 10);
  ctx->mismatch = (*end == ',') ? (int)strtol(end + 1, nullptr, 10) : 0;
  ctx->got = true;
```

The accessor at the end of the HearthClass implementation block:

```cpp
/* The S3.12.1 transport-mismatch flag from a live AT+MTNET? round-trip:
 * true when the device holds a fabric but its active transport is not
 * provisioned. Always a fresh query, like every other network predicate
 * in this library. Older firmware never reports it, so this is false
 * there. */
bool HearthClass::transportMismatch() {
  HearthNetCtx ctx;
  return hearthQueryNet(&ctx) && ctx.mismatch == 1;
}
```

Declaration in `src/Hearth.h` (public section, after `linkUp()`), with the
same comment. Note `hearthQueryNet`/`HearthNetCtx` live in an anonymous
namespace in Hearth.cpp above the class implementations; keep the method
definition below them in the same file.

- [ ] **Step 4: run to verify pass**

Run: `make -C test/host run`
Expected: all binaries pass, including the two new tests.

- [ ] **Step 5: commit** (in the library repo)

```bash
git add src/Hearth.h src/Hearth.cpp test/host/test_hearth.cpp
git commit -m "feat: parse the 0.2.0 mismatch field and expose transportMismatch()"
```

(Body: why the field exists, three-field compatibility; trailers.)

### Task 2: Event 27 to the link-event callback

**Files:**
- Modify: `src/Hearth.h` (`hearthEvent_t` enum, ~line 108)
- Modify: `src/Hearth.cpp` (`hearthDispatchEvt`, ~line 274; its comment ~269)
- Test: `test/host/test_hearth.cpp` (additions)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `HEARTH_TRANSPORT_MISMATCH` appended to `hearthEvent_t`.

- [ ] **Step 1: write the failing tests:**

```cpp
static void test_evt27_raises_link_event() {
  MockStream ms;
  Hearth.begin(ms);
  static int gotHearthEvt;
  static int gotMatterEvt;
  gotHearthEvt = -1;
  gotMatterEvt = 0;
  Hearth.onLinkEvent([](hearthEvent_t e) { gotHearthEvt = (int)e; });
  Matter.onEvent([](matterEvent_t, chip::DeviceLayer::ChipDeviceEvent *) { gotMatterEvt++; });
  ms.injectURC("+MTEVT:27");
  Hearth.poll();
  check("evt27 raised HEARTH_TRANSPORT_MISMATCH",
        gotHearthEvt == (int)HEARTH_TRANSPORT_MISMATCH);
  check("evt27 did not hit the parity callback", gotMatterEvt == 0);
  Matter.onEvent(nullptr);
  Hearth.onLinkEvent(nullptr);
}

static void test_evt28_still_dropped() {
  MockStream ms;
  Hearth.begin(ms);
  static int gotAny;
  gotAny = 0;
  Hearth.onLinkEvent([](hearthEvent_t) { gotAny++; });
  Matter.onEvent([](matterEvent_t, chip::DeviceLayer::ChipDeviceEvent *) { gotAny++; });
  ms.injectURC("+MTEVT:28");
  Hearth.poll();
  check("evt28 dropped silently", gotAny == 0);
  Matter.onEvent(nullptr);
  Hearth.onLinkEvent(nullptr);
}
```

(If `onLinkEvent(nullptr)`/`onEvent(nullptr)` are not accepted resets,
follow whatever cleanup idiom this test file already uses after callback
tests; read it first.)

- [ ] **Step 2: run to verify failure**

Run: `make -C test/host run`
Expected: compile FAILS (`HEARTH_TRANSPORT_MISMATCH` undeclared).

- [ ] **Step 3: implement.** Append to the enum in `src/Hearth.h`:

```cpp
enum hearthEvent_t {
  HEARTH_LINK_UP = 0,
  HEARTH_LINK_DOWN,
  HEARTH_COPROCESSOR_REBOOTED,
  HEARTH_PROTOCOL_ERROR,
  /* Firmware +MTEVT:27 (AT_MT_SPEC.md S3.12.1): the device holds a fabric
   * but its active transport is not provisioned, so it opened a
   * commissioning window. Hearth-specific, so it arrives here and not on
   * the upstream-parity ArduinoMatter::onEvent() surface. Poll
   * Hearth.transportMismatch() for the same state on demand. */
  HEARTH_TRANSPORT_MISMATCH,
};
```

In `hearthDispatchEvt()`, before the table bounds check, special-case bit
27 (the parity table `kEventForBit[27]` itself is NOT grown; the bounds
check stays `bit >= 27` for the table path):

```cpp
  if (bit == 27) {
    /* Transport mismatch is a Hearth extension with no upstream
     * matterEvent_t; it goes to the link-event callback. */
    Hearth.hearthRaiseEvent(HEARTH_TRANSPORT_MISMATCH);
    return;
  }
```

Use the exact raise mechanism the file already uses (`hearthRaiseEvent` at
`src/Hearth.cpp:122`; check its visibility from the anonymous namespace
and route the call the same way the `+MTREADY` path raises
`HEARTH_COPROCESSOR_REBOOTED` at ~:400-408 if a free function cannot call
it directly). Update the dispatch comment at ~:269-273: bits 28-31 are the
reserved-and-dropped range now.

- [ ] **Step 4: run to verify pass**

Run: `make -C test/host run`
Expected: all pass.

- [ ] **Step 5: commit**

```bash
git add src/Hearth.h src/Hearth.cpp test/host/test_hearth.cpp
git commit -m "feat: deliver the transport-mismatch event on the link callback"
```

### Task 3: setTransport(), transport(), and the not-supported mapping

**Files:**
- Modify: `src/Hearth.h` (HearthTransport enum + two method declarations,
  plus the HEARTH_ERR_NOT_SUPPORTED define near the top defines)
- Modify: `src/Hearth.cpp` (implementations + one parser helper)
- Create: `test/host/test_transport.cpp`
- Modify: `test/host/Makefile` (add `test_transport` to the binaries list,
  line 7, following the existing pattern)

**Interfaces:**
- Consumes: `HEARTH_TRANSPORT_MISMATCH` exists (Task 2), but nothing
  functional; tasks are independent.
- Produces:

```cpp
typedef enum {
  HEARTH_TRANSPORT_WIFI = 0,
  HEARTH_TRANSPORT_THREAD = 1,
} HearthTransport;

#define HEARTH_ERR_NOT_SUPPORTED 8  /* the wire's +MTERR:8, named */

bool setTransport(HearthTransport t);
bool transport(HearthTransport *active, HearthTransport *stored);
```

- [ ] **Step 1: write the failing tests** (`test/host/test_transport.cpp`,
new binary; copy the includes/main() skeleton from `test_hearth.cpp`):

```cpp
static void test_set_transport_ok() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT=THREAD", "OK\r\n");
  check("setTransport(THREAD) true", Hearth.setTransport(HEARTH_TRANSPORT_THREAD));
  ms.expect("AT+MTTRANSPORT=WIFI", "OK\r\n");
  check("setTransport(WIFI) true", Hearth.setTransport(HEARTH_TRANSPORT_WIFI));
  check("script drained", ms.scriptDrained());
}

static void test_set_transport_not_supported() {
  /* Single-stack firmware: unknown command answers +MTERR:8 then ERROR. */
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT=THREAD", "+MTERR:8\r\nERROR\r\n");
  check("setTransport false on +MTERR:8", !Hearth.setTransport(HEARTH_TRANSPORT_THREAD));
  check("lastError is HEARTH_ERR_NOT_SUPPORTED",
        Hearth.lastError() == HEARTH_ERR_NOT_SUPPORTED);
}

static void test_set_transport_bad_value_err1() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT=THREAD", "+MTERR:1\r\nERROR\r\n");
  check("setTransport false on +MTERR:1", !Hearth.setTransport(HEARTH_TRANSPORT_THREAD));
  check("lastError is 1", Hearth.lastError() == 1);
}

static void test_transport_query_steady() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT?", "+MTTRANSPORT:WIFI,WIFI\r\nOK\r\n");
  HearthTransport a, s;
  check("transport() true", Hearth.transport(&a, &s));
  check("active WIFI", a == HEARTH_TRANSPORT_WIFI);
  check("stored WIFI", s == HEARTH_TRANSPORT_WIFI);
}

static void test_transport_query_pending_switch() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT?", "+MTTRANSPORT:WIFI,THREAD\r\nOK\r\n");
  HearthTransport a, s;
  check("transport() true", Hearth.transport(&a, &s));
  check("active WIFI", a == HEARTH_TRANSPORT_WIFI);
  check("stored THREAD (pending)", s == HEARTH_TRANSPORT_THREAD);
}

static void test_transport_query_not_supported() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTTRANSPORT?", "+MTERR:8\r\nERROR\r\n");
  HearthTransport a, s;
  check("transport() false on +MTERR:8", !Hearth.transport(&a, &s));
  check("lastError is HEARTH_ERR_NOT_SUPPORTED",
        Hearth.lastError() == HEARTH_ERR_NOT_SUPPORTED);
}
```

- [ ] **Step 2: wire the new binary into `test/host/Makefile` and run**

Run: `make -C test/host run`
Expected: compile FAILS (`HearthTransport` undeclared).

- [ ] **Step 3: implement.** `src/Hearth.h`: the enum and define from the
Interfaces block above (enum before the class; define beside the other
`HEARTH_*` defines), and in the public section:

```cpp
  /*
   * Stage the network transport for the NEXT boot (combined-image
   * firmware, AT_MT_SPEC.md S3.12.2). Returns true when the firmware
   * confirms the setting is stored; nothing changes until the
   * co-processor reboots, and the host owns that reboot. On
   * single-transport firmware the command does not exist and this
   * returns false with lastError() == HEARTH_ERR_NOT_SUPPORTED.
   */
  bool setTransport(HearthTransport t);

  /*
   * Read the active and stored transport. A pending switch shows as
   * active != stored. Same not-supported behavior as setTransport().
   */
  bool transport(HearthTransport *active, HearthTransport *stored);
```

`src/Hearth.cpp`, near the other query helpers:

```cpp
/* "WIFI" / "THREAD" to the enum; wire names are upper-case exact. */
static bool hearthTransportFromName(const char *s, size_t len, HearthTransport *out) {
  if (len == 4 && strncmp(s, "WIFI", 4) == 0) {
    *out = HEARTH_TRANSPORT_WIFI;
    return true;
  }
  if (len == 6 && strncmp(s, "THREAD", 6) == 0) {
    *out = HEARTH_TRANSPORT_THREAD;
    return true;
  }
  return false;
}

struct HearthTransportCtx {
  HearthTransport active;
  HearthTransport stored;
  bool got;
};

void hearthOnTransportLine(const char *line, void *arg) {
  HearthTransportCtx *ctx = (HearthTransportCtx *)arg;
  if (strncmp(line, "+MTTRANSPORT:", 13) != 0) {
    return;
  }
  const char *p = line + 13;
  const char *c1 = strchr(p, ',');
  if (!c1) {
    return;
  }
  HearthTransport a, s;
  if (!hearthTransportFromName(p, (size_t)(c1 - p), &a)) {
    return;
  }
  if (!hearthTransportFromName(c1 + 1, strlen(c1 + 1), &s)) {
    return;
  }
  ctx->active = a;
  ctx->stored = s;
  ctx->got = true;
}
```

and the methods (with the header comments repeated per file convention):

```cpp
bool HearthClass::setTransport(HearthTransport t) {
  const char *name = (t == HEARTH_TRANSPORT_THREAD) ? "THREAD" : "WIFI";
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "AT+MTTRANSPORT=%s", name);
  return hearthCommand(cmd) == 0;
}

bool HearthClass::transport(HearthTransport *active, HearthTransport *stored) {
  HearthTransportCtx ctx;
  ctx.got = false;
  if (hearthCommand("AT+MTTRANSPORT?", hearthOnTransportLine, &ctx) != 0 || !ctx.got) {
    return false;
  }
  *active = ctx.active;
  *stored = ctx.stored;
  return true;
}
```

`hearthCommand()` already stores a positive `+MTERR` code into
`_lastError` (`src/Hearth.cpp:190`), so `+MTERR:8` lands in `lastError()`
with no extra mapping; `HEARTH_ERR_NOT_SUPPORTED` is its name. Check the
helper placement: if `hearthOnTransportLine` sits in the anonymous
namespace, match how `hearthOnNetLine` is declared and used.

- [ ] **Step 4: run to verify pass**

Run: `make -C test/host run`
Expected: all binaries including test_transport pass.

- [ ] **Step 5: commit**

```bash
git add src/Hearth.h src/Hearth.cpp test/host/test_transport.cpp test/host/Makefile
git commit -m "feat: transport selection API for the combined-image firmware"
```

### Task 4: Documentation truth-pass and version bump

**Files:**
- Modify: `README.md` (~lines 70-71 and the surrounding reset paragraph)
- Modify: `src/Hearth.h` (hearthResetCoprocessor doc ~149-150)
- Modify: `src/Hearth.cpp` (decommission ~848-850)
- Modify: `src/HearthCompat.h` (comment block ~35-66)
- Modify: `library.properties` (version 0.1.0 to 0.2.0)
- Test: `test/host/test_hearth.cpp` (one addition)

**Interfaces:** none; documentation and one pinned behavior test.

- [ ] **Step 1: the failing test** (pins what decommission claims to do):

```cpp
static void test_decommission_sends_matter_reset() {
  MockStream ms;
  Hearth.begin(ms);
  ms.expect("AT+MTRESET", "OK\r\n");
  Matter.decommission();
  check("decommission sent AT+MTRESET", ms.scriptDrained());
  check("nothing unexpected", ms.unexpected().empty());
}
```

Run: `make -C test/host run`. Expected: this may already PASS (the
behavior exists; the test pins it). If it passes, that is fine: it is a
regression pin, note it in the report and continue.

- [ ] **Step 2: correct the reset-semantics text.** The factual model
(firmware AT_MT_SPEC.md 3.10, hardware-verified):
`AT+MTRESET` is the MATTER reset: fabrics, credentials and attribute
persistence are erased; the endpoint composition and the stored transport
selection survive. `AT+MTFRESET` additionally erases the composition. The
HARDWARE reset (hearthResetCoprocessor, the pin dance) erases nothing.
On the combined image, network-credential erasure fires only for the
ACTIVE stack; a dormant transport's credentials survive both resets.

Apply it in three places, matching each file's voice:

- `README.md` (the link bring-up paragraph): keep the true claim that the
  pin reset touches nothing; replace the tail clause "only `AT+MTFRESET`
  does" with wording that names both AT commands as the erasing ones,
  e.g.: "The reset does not touch the Matter fabric or the stored
  endpoint composition; those are erased by the `AT+MTRESET` and
  `AT+MTFRESET` commands (the latter also erases the composition), not by
  a reboot."
- `src/Hearth.h` hearthResetCoprocessor comment: same correction to its
  "(only AT+MTFRESET does)" parenthetical.
- `src/Hearth.cpp` decommission(): give it a doc comment:

```cpp
/*
 * Remove the device from its fabric. AT+MTRESET is the firmware's Matter
 * reset (AT_MT_SPEC.md S3.10): it erases the fabrics, credentials and
 * attribute persistence, then reboots. That erasure IS the mechanism
 * here, not a side effect of rebooting. The endpoint composition and,
 * on the combined image, the stored transport selection survive. Note
 * that on the combined image network credentials are erased only for
 * the ACTIVE transport; a dormant transport's credentials survive.
 */
void ArduinoMatter::decommission() {
  Hearth.hearthCommand("AT+MTRESET");
}
```

- [ ] **Step 3: update `src/HearthCompat.h` ~35-66.** The comment asserts
the firmware is always built with WiFi station. Rewrite the stale part for
the three-variant reality: WiFi-only, Thread-only, and the combined image
(one transport active per boot, chosen by AT+MTTRANSPORT). The
`#define CONFIG_ENABLE_CHIPOBLE 1` STAYS, and its justification updates
to: BLE commissioning is resident on all three firmware variants, so the
claim holds regardless of transport. Do not change any define values.

- [ ] **Step 4: bump `library.properties`** version to `0.2.0` (tracks the
firmware contract it now speaks).

- [ ] **Step 5: run everything**

Run: `make -C test/host run`
Expected: all pass.

- [ ] **Step 6: commit**

```bash
git add README.md src/Hearth.h src/Hearth.cpp src/HearthCompat.h library.properties test/host/test_hearth.cpp
git commit -m "docs: reset semantics told straight, and 0.2.0 contract version"
```

(Body: the "only AT+MTFRESET touches the fabric" claim was backwards;
AT+MTRESET is the Matter reset and decommission() depends on exactly
that; trailers.)

### Task 5: Hardware smoke check (bench, short)

**Files:** none committed; evidence in the task report only.

**Interfaces:** consumes the full library as of Task 4.

The library's host tests prove the parsers; this proves the wire. Needs
the bench Challenger (build_b4 currently flashed) and about ten minutes.

- [ ] **Step 1:** compile and upload `examples/MatterOnOffLight` plus a
minimal transport probe sketch (temporary, not committed) that calls
`Hearth.transport()`, `Hearth.setTransport(HEARTH_TRANSPORT_THREAD)` and
`Hearth.transportMismatch()` and prints the results plus
`Hearth.lastError()`.
- [ ] **Step 2: against build_b4 (single-stack)**: expect transport() and
setTransport() to return false with lastError() == 8, and
transportMismatch() false. This validates the not-supported path against
real firmware, not just MockStream.
- [ ] **Step 3: against build_combined**: reflash the C6 with the combined
image (`python3 fw/flash.py --build-dir build_combined --port /dev/serial/by-id/usb-iLabs_Challenger_2350_WiFi_BLE_83C29C7F51EC55B4-if00 --bridge espnow`
from the firmware repo), factory-fresh it, and expect: transport() reports
WIFI,WIFI; setTransport(THREAD) true; transport() reports WIFI,THREAD
(pending); reboot the C6 (Hearth.hearthResetCoprocessor()); transport()
reports THREAD,THREAD. Then set back to WIFI, reboot, confirm, and restore
the bench: reflash build_b4, factory-fresh, composition staged
(AT+MTEP=0x0100 via the harness or a probe).
- [ ] **Step 4:** record all evidence in the task report. No library
commit unless a defect was found and fixed (fixes go through the normal
review loop).
