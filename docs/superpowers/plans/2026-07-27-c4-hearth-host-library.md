# C4: iLabs Hearth host library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An arduino-pico library that lets an unmodified arduino-esp32 Matter
sketch run on a Challenger RP2350, driving an ESP32-C6 running Hearth firmware
over the `AT+MT` UART protocol.

**Architecture:** The C6 holds all Matter state. The library is a façade: it
mirrors arduino-esp32's class surface verbatim, translates each call into an
`AT+MT` command line, and demultiplexes URCs back into the sketch's callbacks.
`Matter.begin()` reconciles the sketch's declared endpoint composition against
the one persisted on the C6 (§5.4 of the design spec) and adopts the returned
endpoint IDs.

**Tech Stack:** C++11 (arduino-pico / RP2350), Arduino `Stream` for transport,
plain g++ and `assert`-style checks for host unit tests. No external
dependencies, no test framework.

**Plan lives here, code lives elsewhere.** The plan and spec stay in
`iLabs_AT_Hearth/docs/` with the rest of the decision record. All code paths
below are relative to the **library repo**:
`/mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/Dropbox/Arduino/libraries/iLabs_Hearth`
(also reachable as `~/Arduino/libraries/iLabs_Hearth`). Task 10 is the only one
that touches `iLabs_AT_Hearth`.

## Global Constraints

Every task's requirements implicitly include this section.

- **Identity is Hearth. Never name the repo, package or artifact "Matter".**
  Matter-named symbols are a closed set defined by upstream arduino-esp32; we
  implement them and never extend them. Anything we invent gets a Hearth name.
  Source: `docs/superpowers/specs/2026-07-27-c4-host-library-naming-design.md`.
- **No em dashes** anywhere: prose, comments, commit messages. Use a colon,
  comma, parentheses or a full stop.
- **Reference version of the parity surface:** arduino-esp32 **3.3.8**, at
  `~/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Matter/`. Every
  signature this plan reproduces was read from there. When in doubt, read that
  tree, not this plan.
- **Transport:** UART `115200 8N1`. Commands end with CRLF. Responses and URCs
  end with CRLF. **One command in flight**: wait for `OK`/`ERROR` before
  sending the next. URCs may arrive at any time, including between a command
  and its terminal response.
- **Error grammar:** bare `ERROR` means the command form was wrong.
  `+MTERR:<n>` on the line before `ERROR` means: `1` bad parameter, `2` unknown
  endpoint, `3` unknown cluster, `4` unknown attribute, `5` unsupported
  attribute type, `6` unknown device type, `7` NVS failure, `8` unknown command.
- **Device type IDs**, read from
  `~/esp/esp-matter/components/esp_matter/data_model/esp_matter_endpoint.h` on
  2026-07-27 and not to be transcribed from memory:

  | Class | Device type | ID |
  |---|---|---|
  | `MatterOnOffLight` | `on_off_light` | `0x0100` |
  | `MatterDimmableLight` | `dimmable_light` | `0x0101` |
  | `MatterColorTemperatureLight` | `color_temperature_light` | `0x010C` |
  | `MatterTemperatureSensor` | `temperature_sensor` | `0x0302` |

- **Cluster and attribute IDs used by these four classes:**

  | Cluster | ID | Attribute | ID |
  |---|---|---|---|
  | OnOff | `0x0006` | `OnOff` | `0x0000` |
  | LevelControl | `0x0008` | `CurrentLevel` | `0x0000` |
  | ColorControl | `0x0300` | `ColorTemperatureMireds` | `0x0007` |
  | TemperatureMeasurement | `0x0402` | `MeasuredValue` | `0x0000` |

- **C++ style:** match arduino-esp32's Matter library, since half these files
  are mirrors of it. 2-space indent, `camelCase` methods, `_leadingUnderscore`
  private members. This differs from the firmware's C style deliberately.
- **Commit after every task.** Commits explain *why* and name the thing that
  would otherwise be rediscovered.

## A correction this plan makes to the spec

Design spec §5.4 says `Matter.begin()` compares against "the sequence the sketch
declared through its **endpoint constructors**". This plan registers the
declaration in **`endpoint.begin()`** instead, for two reasons:

1. Global constructor order across translation units is unspecified in C++.
   Endpoint ID assignment depends on declaration order, and §5.3 calls ID
   stability a load-bearing invariant. Resting it on static init order would
   break it for any sketch spanning more than one `.cpp`.
2. It matches the documented upstream idiom. Every arduino-esp32 example calls
   each `endpoint.begin(...)` in `setup()` and then, per its own comment,
   `Matter.begin()` as the *"Last step, after all EndPoints are initialized"`.

Consequence: an endpoint whose `begin()` runs **after** `Matter.begin()` is not
in the reconciled composition. Task 5 makes this a reported error rather than a
silent one.

## File Structure

```
iLabs_Hearth/
  library.properties            package identity (Task 1)
  keywords.txt                  IDE syntax highlighting (Task 9)
  README.md                     (Task 9)
  src/
    Hearth.h                    canonical umbrella: ArduinoMatter, HearthClass,
                                the two globals, both event enums (Tasks 4, 5)
    Hearth.cpp                  their implementations, incl. reconcile (Tasks 4, 5)
    Matter.h                    one-line shim over Hearth.h (Task 1)
    HearthLink.h/.cpp           AT line protocol: framing, +MTERR, URC demux (Task 1)
    HearthCompat.h              esp_matter types the parity surface leaks (Task 2)
    MatterEndPoint.h/.cpp       base class + declaration registry (Task 3)
    MatterEndpoints/
      MatterOnOffLight.h/.cpp             (Task 6)
      MatterDimmableLight.h/.cpp          (Task 7)
      MatterColorTemperatureLight.h/.cpp  (Task 8)
      MatterTemperatureSensor.h/.cpp      (Task 8)
  test/host/
    Makefile                    `make -C test/host run` (Task 1)
    ArduinoShim.h               Stream, Print, String, millis for g++ (Task 1)
    MockStream.h                scripted AT exchange double (Task 1)
    test_hearthlink.cpp         (Task 1)
    test_attrval.cpp            (Task 2)
    test_endpoint.cpp           (Task 3)
    test_hearth.cpp             (Task 4)
    test_reconcile.cpp          (Task 5)
    test_onofflight.cpp         (Task 6)
    test_dimmable.cpp           (Task 7)
    test_colortemp_sensor.cpp   (Task 8)
  examples/                     upstream examples, byte-identical (Task 9)
```

`Hearth.h` carries both globals rather than splitting them because spec §5 fixes
that layout, and because upstream's own `Matter.h` is a single 227-line header
of the same shape.

---

### Task 1: HearthLink line protocol, plus the host test harness

The AT client everything else sits on. It is a generalisation of `ATLink` from
`~/Data/src/git/iLabs_ESP-NOW/src/ATLink.{h,cpp}` (125 lines), which is
`AT+EN`-specific in exactly four places: the `+EN…` URC prefixes, the
`+ENERR:` prefix, the `channel` argument to `begin()`, and the
`ILABS_ESPNOW_*` macro names.

**Do not modify `iLabs_ESP-NOW`.** Copy and generalise. That library is shipped,
and CLAUDE.md's rule that shared code needs both firmwares retested applies to
its host twin for the same reason. De-duplicating the two is a follow-up, listed
in Task 10.

**Files:**
- Create: `library.properties`
- Create: `src/HearthLink.h`, `src/HearthLink.cpp`
- Create: `src/Matter.h`
- Create: `test/host/Makefile`, `test/host/ArduinoShim.h`, `test/host/MockStream.h`
- Test: `test/host/test_hearthlink.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  class HearthLink {
  public:
    typedef void (*LineCb)(const char *line, void *arg);
    HearthLink();
    void begin(Stream &serial);
    bool started() const;
    Stream *stream() const;
    /* 0 on OK, >0 the +MTERR code, -1 plain ERROR, -2 timeout or not started */
    int command(const char *cmd, LineCb onLine = nullptr, void *arg = nullptr,
                uint32_t timeout_ms = 0);
    void poll();
    void onURC(LineCb cb, void *arg);
    static bool isAsyncURC(const char *line);
  };
  ```

- [ ] **Step 1: Create `library.properties`**

```
name=iLabs Hearth
version=0.1.0
author=iLabs Electronics
maintainer=Pontus Oldberg <pontus.oldberg@gmail.com>
sentence=Arduino Matter API for RP2040/RP2350 hosts, bridged to an ESP32-C6 running iLabs Hearth firmware over the iLabs AT+MT UART protocol.
paragraph=Source-compatible with the arduino-esp32 Matter class API, so an unmodified sketch builds and runs on a Challenger. The Matter stack runs on an ESP32-C6 flashed with the iLabs Hearth firmware; this library speaks that AT protocol over a Serial link. Call Hearth.begin(Serial1) in setup() to override the default link, then Matter.begin() as the last step after all endpoints are initialized. iLabs Challenger+ platform.
category=Communication
url=https://github.com/PontusO/iLabs_Hearth
architectures=rp2040
includes=Matter.h
```

- [ ] **Step 2: Create `src/Matter.h`, the shim**

```cpp
/*
 * Matter.h - source-compatibility shim.
 *
 * Exists so an unmodified arduino-esp32 sketch's `#include <Matter.h>`
 * resolves. The library's own canonical header is Hearth.h; see
 * iLabs_AT_Hearth/docs/superpowers/specs/2026-07-27-c4-host-library-naming-design.md
 * for why the product is never named Matter.
 */

#pragma once
#include "Hearth.h"
```

- [ ] **Step 3: Create the host Arduino shim, `test/host/ArduinoShim.h`**

Enough of Arduino to compile the library under g++. `millis()` is a settable
counter so timeout tests do not sleep.

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline void delay(uint32_t ms) { g_millis += ms; }

class String : public std::string {
public:
  String() {}
  String(const char *s) : std::string(s ? s : "") {}
  const char *c_str() const { return std::string::c_str(); }
};

class Print {
public:
  virtual size_t write(uint8_t c) = 0;
  size_t write(const char *s) {
    size_t n = 0;
    while (*s) { n += write((uint8_t)*s++); }
    return n;
  }
  size_t print(const char *s) { return write(s); }
  size_t println(const char *s) { return write(s) + write("\r\n"); }
  size_t printf(const char *fmt, ...);
};

class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual void flush() {}
};
```

Add `ArduinoShim.cpp`-free definitions by declaring `uint32_t g_millis = 0;`
and the `Print::printf` body in `test/host/ArduinoShim.cpp`.

- [ ] **Step 4: Create `test/host/MockStream.h`, the scripted double**

```cpp
#pragma once
#include "ArduinoShim.h"
#include <deque>
#include <vector>

/*
 * Scripted AT peer. expect() queues one command the library is required to
 * send next, together with the bytes to hand back. injectURC() queues a line
 * the library must be able to receive without any command in flight.
 */
class MockStream : public Stream {
public:
  void expect(const std::string &cmd, const std::string &response) {
    _script.push_back({cmd, response});
  }
  void injectURC(const std::string &line) { _rx += line + "\r\n"; }

  bool scriptDrained() const { return _script.empty(); }
  const std::vector<std::string> &unexpected() const { return _unexpected; }

  size_t write(uint8_t c) override {
    if (c == '\n') { return 1; }
    if (c == '\r') { deliver(_txline); _txline.clear(); return 1; }
    _txline.push_back((char)c);
    return 1;
  }
  int available() override { return (int)_rx.size(); }
  int read() override {
    if (_rx.empty()) { return -1; }
    int c = (unsigned char)_rx.front();
    _rx.erase(0, 1);
    return c;
  }
  int peek() override { return _rx.empty() ? -1 : (unsigned char)_rx.front(); }

private:
  struct Exchange { std::string cmd, response; };
  void deliver(const std::string &sent) {
    if (_script.empty() || _script.front().cmd != sent) {
      _unexpected.push_back(sent);
      return;
    }
    _rx += _script.front().response;
    _script.pop_front();
  }
  std::deque<Exchange> _script;
  std::vector<std::string> _unexpected;
  std::string _txline, _rx;
};
```

- [ ] **Step 5: Create `test/host/Makefile`**

Mirrors `iLabs_AT_Hearth/test/host/Makefile`: plain compiler, no framework.

```make
# Host unit tests for the transport and logic layers of iLabs Hearth. No
# Arduino core and no hardware: ArduinoShim.h stands in for Arduino.h.
# Run with `make -C test/host run`.

CXXFLAGS = -std=c++11 -Wall -Wextra -Werror -g -I. -I../../src
SRC = ../../src/HearthLink.cpp
TESTS = test_hearthlink

all: $(TESTS)

test_hearthlink: test_hearthlink.cpp ArduinoShim.cpp $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

run: all
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all run clean
```

Note for later tasks: each adds its sources to `SRC` and its binary to `TESTS`.

- [ ] **Step 6: Write the failing test, `test/host/test_hearthlink.cpp`**

```cpp
/*
 * Host unit tests for the AT line protocol client. No framework: the library
 * has no test dependency and needs none. Build with `make -C test/host run`.
 */
#include <stdio.h>
#include "ArduinoShim.h"
#include "MockStream.h"
#include "HearthLink.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

static void collect(const char *line, void *arg) {
  ((std::string *)arg)->append(line).append(";");
}

static void test_ok(void) {
  MockStream s;
  s.expect("AT", "OK\r\n");
  HearthLink link;
  link.begin(s);
  check("plain OK returns 0", link.command("AT") == 0);
  check("script drained", s.scriptDrained());
}

static void test_coded_error(void) {
  MockStream s;
  s.expect("AT+MTATTR=9,6,0", "+MTERR:2\r\nERROR\r\n");
  HearthLink link;
  link.begin(s);
  check("+MTERR:2 is returned as 2", link.command("AT+MTATTR=9,6,0") == 2);
}

static void test_plain_error(void) {
  MockStream s;
  s.expect("AT+MTBOGUS", "ERROR\r\n");
  HearthLink link;
  link.begin(s);
  check("bare ERROR returns -1", link.command("AT+MTBOGUS") == -1);
}

static void test_intermediate_lines(void) {
  MockStream s;
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\n+MTEP:1,2,0x0302\r\nOK\r\n");
  HearthLink link;
  link.begin(s);
  std::string got;
  check("query returns 0", link.command("AT+MTEP?", collect, &got) == 0);
  check("both result lines delivered",
        got == "+MTEP:0,1,0x0100;+MTEP:1,2,0x0302;");
}

static void test_urc_during_command(void) {
  MockStream s;
  /* A URC lands between the command and its terminal OK. It must go to the
   * URC handler, not to the command's line callback. */
  s.expect("AT+MTCODES?",
           "+MTEVT:3\r\n+MTCODES:MT:Y.K9042C00KA0648G00,34970112332\r\nOK\r\n");
  HearthLink link;
  link.begin(s);
  std::string urcs, lines;
  link.onURC(collect, &urcs);
  check("command still returns 0", link.command("AT+MTCODES?", collect, &lines) == 0);
  check("URC routed to the URC handler", urcs == "+MTEVT:3;");
  check("only the result line reached the command callback",
        lines == "+MTCODES:MT:Y.K9042C00KA0648G00,34970112332;");
}

static void test_poll_dispatches_urc(void) {
  MockStream s;
  HearthLink link;
  link.begin(s);
  std::string urcs;
  link.onURC(collect, &urcs);
  s.injectURC("+MTATTR:1,6,0,1");
  link.poll();
  check("poll dispatches a pending URC", urcs == "+MTATTR:1,6,0,1;");
}

static void test_timeout(void) {
  MockStream s;
  s.expect("AT", "");  /* peer says nothing */
  HearthLink link;
  link.begin(s);
  check("silence returns -2", link.command("AT", nullptr, nullptr, 50) == -2);
}

static void test_not_started(void) {
  HearthLink link;
  check("command before begin returns -2", link.command("AT") == -2);
}

int main(void) {
  printf("\n===== HearthLink line protocol tests =====\n");
  test_ok();
  test_coded_error();
  test_plain_error();
  test_intermediate_lines();
  test_urc_during_command();
  test_poll_dispatches_urc();
  test_timeout();
  test_not_started();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 7: Run the test and confirm it fails to build**

Run: `make -C test/host run`
Expected: compile error, `HearthLink.h: No such file or directory`.

- [ ] **Step 8: Write `src/HearthLink.h`**

Copy `~/Data/src/git/iLabs_ESP-NOW/src/ATLink.h`, then:
rename the class to `HearthLink`; drop the `channel` parameter and the `_chan`
member from `begin()`; rename `ILABS_ESPNOW_LINE_MAX` to `HEARTH_LINE_MAX` and
`ILABS_ESPNOW_CMD_TIMEOUT_MS` to `HEARTH_CMD_TIMEOUT_MS`; rewrite the header
comment for `AT+MT`. `HEARTH_LINE_MAX` can be `256`, since the longest line in
the protocol is a `+MTCODES:` pair of a QR payload and an 11-digit manual code.

- [ ] **Step 9: Write `src/HearthLink.cpp`**

Copy `ATLink.cpp` and change the four protocol-specific points:

```cpp
bool HearthLink::isAsyncURC(const char *line) {
  return strncmp(line, "+MTEVT", 6) == 0 || strncmp(line, "+MTATTR", 7) == 0
      || strncmp(line, "+MTIDENT", 8) == 0 || strncmp(line, "+MTREADY", 8) == 0;
}
```

and, in the terminal-response scan, `+MTERR:` in place of `+ENERR:`.

**`+MTATTR` is deliberately in that list even though it is also a query result.**
A `AT+MTATTR=<ep>,<cl>,<attr>` read answers with a `+MTATTR:` line, so the
command path must claim it before `isAsyncURC` sees it. `command()` already
routes intermediate lines to `onLine` when a command is in flight, so the rule
is: in flight, `+MTATTR` is a result; idle, it is a URC. Verify the copied
`command()` loop preserves that ordering, because the ESP-NOW original has no
URC that doubles as a result and never exercised it.

- [ ] **Step 10: Run the tests and confirm they pass**

Run: `make -C test/host run`
Expected: `RESULT: 13 passed, 0 failed`.

- [ ] **Step 11: Commit**

```bash
cd ~/Arduino/libraries/iLabs_Hearth
git add library.properties src/HearthLink.h src/HearthLink.cpp src/Matter.h test/
git commit -m "feat: AT+MT line protocol client and the host test harness

Generalised from iLabs_ESP-NOW's ATLink, which is AT+EN-specific in four
places: the URC prefixes, the +ENERR prefix, begin()'s channel argument, and
its macro names. Copied rather than shared: that library is shipped, and the
retest cost of touching it is the host-side twin of the at_core rule.

+MTATTR is both a query result and a URC, which AT+EN has no equivalent of, so
the in-flight-versus-idle routing is now covered by a test the original never
needed."
```

---

### Task 2: HearthCompat, the esp_matter types the parity surface leaks

`MatterEndPoint`'s public helpers take `esp_matter_attr_val_t *`, and
`attributeChangeCB` is part of the surface a sketch may override. Those types
come from ESP-IDF, which does not exist on RP2350. This is the same problem
`iLabs_ESP-NOW` solved with `ilabs_espnow_compat.h`: reproduce only the types,
constants and macros the public API and the official examples actually touch.

Per the naming rule the *file* is ours and is Hearth-named; the *types* inside
keep their upstream names, because they are interop symbols.

**Files:**
- Create: `src/HearthCompat.h`
- Test: `test/host/test_attrval.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  typedef enum { ESP_MATTER_VAL_TYPE_INVALID = 0, ESP_MATTER_VAL_TYPE_BOOLEAN,
                 ESP_MATTER_VAL_TYPE_INTEGER, ESP_MATTER_VAL_TYPE_INT8,
                 ESP_MATTER_VAL_TYPE_UINT8, ESP_MATTER_VAL_TYPE_INT16,
                 ESP_MATTER_VAL_TYPE_UINT16, ESP_MATTER_VAL_TYPE_INT32,
                 ESP_MATTER_VAL_TYPE_UINT32, ESP_MATTER_VAL_TYPE_ENUM8,
                 ESP_MATTER_VAL_TYPE_BITMAP8 } esp_matter_val_type_t;

  typedef struct { esp_matter_val_type_t type;
                   union { bool b; int32_t i; uint32_t u; } val; } esp_matter_attr_val_t;

  esp_matter_attr_val_t esp_matter_bool(bool v);
  esp_matter_attr_val_t esp_matter_uint8(uint8_t v);
  esp_matter_attr_val_t esp_matter_uint16(uint16_t v);
  esp_matter_attr_val_t esp_matter_int16(int16_t v);

  /* Flatten to the single integer AT+MTATTR carries; false if not carryable. */
  bool hearthAttrValToLong(const esp_matter_attr_val_t &v, long *out);
  /* Rebuild a typed value from an AT integer, given the target type. */
  esp_matter_attr_val_t hearthAttrValFromLong(esp_matter_val_type_t t, long v);

  namespace chip { namespace DeviceLayer { struct ChipDeviceEvent { uint8_t bit; int detail; }; } }
  ```

The type list is deliberately short. CLAUDE.md records a census of all 20
arduino-esp32 endpoint classes finding only `u8`, `u16`, `bool`, `i16`, `u32`
and one array, and `AT+MTATTR` carries integers and booleans only. An
uncarryable type is `+MTERR:5` on the wire and `false` from
`hearthAttrValToLong` here.

`ChipDeviceEvent` is a stub because `ArduinoMatter::matterEventCB` takes a
pointer to it. Sketches pass it through without dereferencing; we populate the
bit and detail we actually have from `+MTEVT`.

- [ ] **Step 1: Write the failing test, `test/host/test_attrval.cpp`**

```cpp
#include <stdio.h>
#include "ArduinoShim.h"
#include "HearthCompat.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

static void test_bool_roundtrip(void) {
  long out = -1;
  check("bool true flattens to 1", hearthAttrValToLong(esp_matter_bool(true), &out) && out == 1);
  check("bool false flattens to 0", hearthAttrValToLong(esp_matter_bool(false), &out) && out == 0);
  esp_matter_attr_val_t v = hearthAttrValFromLong(ESP_MATTER_VAL_TYPE_BOOLEAN, 1);
  check("1 rebuilds as bool true", v.type == ESP_MATTER_VAL_TYPE_BOOLEAN && v.val.b);
}

static void test_unsigned_roundtrip(void) {
  long out = 0;
  check("uint8 255 flattens", hearthAttrValToLong(esp_matter_uint8(255), &out) && out == 255);
  check("uint16 65535 flattens", hearthAttrValToLong(esp_matter_uint16(65535), &out) && out == 65535);
  esp_matter_attr_val_t v = hearthAttrValFromLong(ESP_MATTER_VAL_TYPE_UINT8, 64);
  check("64 rebuilds as uint8", v.type == ESP_MATTER_VAL_TYPE_UINT8 && v.val.u == 64);
}

static void test_signed_negative(void) {
  long out = 0;
  /* -12.34 C is the TemperatureMeasurement MeasuredValue -1234. Signedness is
   * the one thing a naive unsigned-only codec gets wrong, so it is asserted. */
  check("int16 -1234 keeps its sign",
        hearthAttrValToLong(esp_matter_int16((int16_t)-1234), &out) && out == -1234);
  esp_matter_attr_val_t v = hearthAttrValFromLong(ESP_MATTER_VAL_TYPE_INT16, -1234);
  check("-1234 rebuilds as int16", v.type == ESP_MATTER_VAL_TYPE_INT16 && v.val.i == -1234);
}

static void test_uncarryable(void) {
  long out = 0;
  esp_matter_attr_val_t v; v.type = ESP_MATTER_VAL_TYPE_INVALID; v.val.i = 0;
  check("an uncarryable type is refused", !hearthAttrValToLong(v, &out));
}

int main(void) {
  printf("\n===== attribute value codec tests =====\n");
  test_bool_roundtrip();
  test_unsigned_roundtrip();
  test_signed_negative();
  test_uncarryable();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add the test to the Makefile**

In `test/host/Makefile`, add `test_attrval` to `TESTS` and the rule:

```make
test_attrval: test_attrval.cpp ArduinoShim.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
```

- [ ] **Step 3: Run and confirm it fails**

Run: `make -C test/host run`
Expected: `HearthCompat.h: No such file or directory`.

- [ ] **Step 4: Write `src/HearthCompat.h`**

Header-only, with the `inline` constructors and the two codec functions from
the Interfaces block. `hearthAttrValToLong` returns `false` for
`ESP_MATTER_VAL_TYPE_INVALID` and any type not in the enum; signed types read
`val.i`, unsigned and boolean read `val.u` / `val.b`.

- [ ] **Step 5: Run and confirm it passes**

Run: `make -C test/host run`
Expected: `RESULT: 9 passed, 0 failed` for this binary, and Task 1's still green.

- [ ] **Step 6: Commit**

```bash
git add src/HearthCompat.h test/host/test_attrval.cpp test/host/Makefile
git commit -m "feat: host-side esp_matter attribute value types

MatterEndPoint's public helpers take esp_matter_attr_val_t, so the parity
surface leaks an ESP-IDF type onto a host that has no ESP-IDF. Same shape as
iLabs_ESP-NOW's ilabs_espnow_compat.h: reproduce only what the public API
touches. The type list is short because AT+MTATTR carries integers and
booleans only.

The signed case is tested explicitly: TemperatureMeasurement MeasuredValue is
an int16 that goes negative, and an unsigned-only codec passes every other
test in this file."
```

---

### Task 3: MatterEndPoint base class and the declaration registry

**Files:**
- Create: `src/MatterEndPoint.h`, `src/MatterEndPoint.cpp`
- Test: `test/host/test_endpoint.cpp`

**Interfaces:**
- Consumes: `HearthLink` (Task 1), `HearthCompat.h` (Task 2).
- Produces:
  ```cpp
  class MatterEndPoint {
  public:
    enum attrOperation_t { ATTR_SET = false, ATTR_UPDATE = true };
    using EndPointIdentifyCB = std::function<bool(bool)>;

    virtual bool attributeChangeCB(uint16_t endpoint_id, uint32_t cluster_id,
                                   uint32_t attribute_id, esp_matter_attr_val_t *val) = 0;
    uint16_t getEndPointId();
    void setEndPointId(uint16_t ep);
    bool getAttributeVal(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *attrVal);
    bool setAttributeVal(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *attrVal);
    bool updateAttributeVal(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *attrVal);
    bool endpointIdentifyCB(uint16_t endpoint_id, bool identifyIsEnabled);
    void onIdentify(EndPointIdentifyCB cb);

    /* Hearth additions, not part of the upstream surface. */
    static bool hearthDeclare(MatterEndPoint *ep, uint32_t deviceTypeId);
    static uint8_t hearthDeclaredCount();
    static MatterEndPoint *hearthDeclaredAt(uint8_t index);
    static uint32_t hearthDeclaredTypeAt(uint8_t index);
    static void hearthClearDeclarations();
    static MatterEndPoint *hearthFindByEndPointId(uint16_t ep);
  protected:
    uint16_t endpoint_id = 0;
    EndPointIdentifyCB _onEndPointIdentifyCB = nullptr;
  };
  ```

The `hearth*` statics are Hearth-named because we invented them, per the naming
rule. `createSecondaryNetworkInterface()` and `getSecondaryNetworkEndPointId()`
from upstream are **not implemented**: they exist for devices with two network
interfaces, the C6 image has one, and Task 9 records the gap in the README.

`setAttributeVal` writes with `,0` (no report) and `updateAttributeVal` writes
with `,1` (reported), which is exactly what spec §3.8's mode parameter was added
for. Registry capacity is `HEARTH_MAX_ENDPOINTS`, defaulting to `16` to match
the firmware's `MT_COMP_MAX_ENDPOINTS`.

- [ ] **Step 1: Write the failing test, `test/host/test_endpoint.cpp`**

```cpp
#include <stdio.h>
#include "ArduinoShim.h"
#include "MockStream.h"
#include "Hearth.h"
#include "MatterEndPoint.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

/* Concrete stand-in: the base class is abstract. */
class TestEndPoint : public MatterEndPoint {
public:
  int changes = 0;
  bool attributeChangeCB(uint16_t, uint32_t, uint32_t, esp_matter_attr_val_t *) override {
    changes++;
    return true;
  }
};

static void test_write_modes(void) {
  MockStream s;
  s.expect("AT+MTATTR=1,6,0,1,0", "OK\r\n");
  s.expect("AT+MTATTR=1,6,0,1,1", "OK\r\n");
  Hearth.begin(s);
  TestEndPoint ep;
  ep.setEndPointId(1);
  esp_matter_attr_val_t v = esp_matter_bool(true);
  check("setAttributeVal writes mode 0", ep.setAttributeVal(0x0006, 0x0000, &v));
  check("updateAttributeVal writes mode 1", ep.updateAttributeVal(0x0006, 0x0000, &v));
  check("script drained", s.scriptDrained());
}

static void test_read(void) {
  MockStream s;
  s.expect("AT+MTATTR=1,6,0", "+MTATTR:1,6,0,1\r\nOK\r\n");
  Hearth.begin(s);
  TestEndPoint ep;
  ep.setEndPointId(1);
  esp_matter_attr_val_t v = esp_matter_bool(false);
  check("read succeeds", ep.getAttributeVal(0x0006, 0x0000, &v));
  check("read returns the reported value", v.val.b == true);
}

static void test_unknown_endpoint_reports_code(void) {
  MockStream s;
  s.expect("AT+MTATTR=9,6,0,1,1", "+MTERR:2\r\nERROR\r\n");
  Hearth.begin(s);
  TestEndPoint ep;
  ep.setEndPointId(9);
  esp_matter_attr_val_t v = esp_matter_bool(true);
  check("a failed write returns false", !ep.updateAttributeVal(0x0006, 0x0000, &v));
  check("the +MTERR code is retained", Hearth.lastError() == 2);
}

static void test_declaration_order(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint a, b;
  check("first declaration accepted", MatterEndPoint::hearthDeclare(&a, 0x0100));
  check("second declaration accepted", MatterEndPoint::hearthDeclare(&b, 0x0302));
  check("count is 2", MatterEndPoint::hearthDeclaredCount() == 2);
  check("order preserved", MatterEndPoint::hearthDeclaredTypeAt(0) == 0x0100
                        && MatterEndPoint::hearthDeclaredTypeAt(1) == 0x0302);
}

static void test_lookup_by_endpoint_id(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint a;
  MatterEndPoint::hearthDeclare(&a, 0x0100);
  a.setEndPointId(4);
  check("lookup finds the endpoint", MatterEndPoint::hearthFindByEndPointId(4) == &a);
  check("lookup misses cleanly", MatterEndPoint::hearthFindByEndPointId(7) == nullptr);
}

static void test_identify_callback(void) {
  TestEndPoint ep;
  bool seen = false, state = false;
  ep.onIdentify([&](bool on) { seen = true; state = on; return true; });
  ep.endpointIdentifyCB(1, true);
  check("identify callback fires", seen && state);
}

int main(void) {
  printf("\n===== MatterEndPoint tests =====\n");
  test_write_modes();
  test_read();
  test_unknown_endpoint_reports_code();
  test_declaration_order();
  test_lookup_by_endpoint_id();
  test_identify_callback();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C test/host run`
Expected: fails to build, `MatterEndPoint.h` and `Hearth.h` missing. `Hearth`
arrives in Task 4; to keep this task independently runnable, implement the
minimal `Hearth` global (a `HearthLink` plus `begin(Stream&)` and `lastError()`)
here and let Task 4 extend it.

- [ ] **Step 3: Write `src/MatterEndPoint.h` and `src/MatterEndPoint.cpp`**

Command construction, showing the mode split that is the point of the class:

```cpp
bool MatterEndPoint::setAttributeVal(uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *attrVal) {
  return hearthWriteAttr(cluster_id, attribute_id, attrVal, 0);
}

bool MatterEndPoint::updateAttributeVal(uint32_t cluster_id, uint32_t attribute_id,
                                        esp_matter_attr_val_t *attrVal) {
  return hearthWriteAttr(cluster_id, attribute_id, attrVal, 1);
}

bool MatterEndPoint::hearthWriteAttr(uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *attrVal, int mode) {
  long v;
  if (attrVal == nullptr || !hearthAttrValToLong(*attrVal, &v)) {
    Hearth.hearthSetError(5);  /* the wire's "type not carryable" code */
    return false;
  }
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+MTATTR=%u,%lu,%lu,%ld,%d",
           (unsigned)endpoint_id, (unsigned long)cluster_id,
           (unsigned long)attribute_id, v, mode);
  return Hearth.hearthCommand(cmd) == 0;
}
```

- [ ] **Step 4: Run and confirm it passes**

Run: `make -C test/host run`
Expected: `RESULT: 11 passed, 0 failed` for this binary.

- [ ] **Step 5: Commit**

```bash
git add src/MatterEndPoint.h src/MatterEndPoint.cpp test/host/test_endpoint.cpp test/host/Makefile
git commit -m "feat: MatterEndPoint base class and the declaration registry

setAttributeVal maps to AT+MTATTR mode 0 and updateAttributeVal to mode 1,
which is the pairing the mode parameter was added for in C2: a host reflecting
a controller-driven change must not echo it back to the fabric.

Registration is a static ordered registry rather than construction order,
because endpoint IDs are assigned from declaration order and static init order
across translation units is unspecified. Endpoint ID stability is load-bearing
per design spec 5.3.

createSecondaryNetworkInterface is deliberately absent: the C6 image has one
network interface."
```

---

### Task 4: The Hearth global, link configuration and diagnostics

**Files:**
- Create: `src/Hearth.h`, `src/Hearth.cpp` (extending the minimal version from Task 3)
- Test: `test/host/test_hearth.cpp`

**Interfaces:**
- Consumes: `HearthLink` (Task 1).
- Produces:
  ```cpp
  enum hearthEvent_t {
    HEARTH_LINK_UP = 0, HEARTH_LINK_DOWN, HEARTH_COPROCESSOR_REBOOTED,
    HEARTH_PROTOCOL_ERROR,
  };

  class HearthClass {
  public:
    using hearthEventCB = std::function<void(hearthEvent_t)>;
    void begin(Stream &serial, unsigned long baud = 115200);
    bool linkUp();
    int lastError();                 /* last +MTERR code, 0 if none */
    void onLinkEvent(hearthEventCB cb);
    void poll();
    String firmwareVersion();        /* AT+MTVER? */
    /* internal, used by the Matter-named layer */
    int hearthCommand(const char *cmd, HearthLink::LineCb onLine = nullptr, void *arg = nullptr);
    void hearthSetError(int code);
    HearthLink &link();
  };
  extern HearthClass Hearth;
  ```

**The default link.** `begin()` may be skipped entirely, in which case the
library uses `HEARTH_DEFAULT_SERIAL` at `115200`. Define it as `Serial1` behind
`#ifndef` so a board variant can override it at build time. Task 11 confirms
the actual Challenger pins on hardware; until then `Serial1` is the documented
assumption, not a verified fact.

- [ ] **Step 1: Write the failing test, `test/host/test_hearth.cpp`**

```cpp
#include <stdio.h>
#include "ArduinoShim.h"
#include "MockStream.h"
#include "Hearth.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

static void test_version(void) {
  MockStream s;
  s.expect("AT+MTVER?", "+MTVER:0.4.0\r\nOK\r\n");
  Hearth.begin(s);
  check("firmwareVersion parses +MTVER", Hearth.firmwareVersion() == String("0.4.0"));
}

static void test_last_error_cleared_on_success(void) {
  MockStream s;
  s.expect("AT+MTBOGUS", "+MTERR:8\r\nERROR\r\n");
  s.expect("AT", "OK\r\n");
  Hearth.begin(s);
  Hearth.hearthCommand("AT+MTBOGUS");
  check("error code retained", Hearth.lastError() == 8);
  Hearth.hearthCommand("AT");
  check("a later success clears it", Hearth.lastError() == 0);
}

static void test_reboot_urc_raises_event(void) {
  MockStream s;
  Hearth.begin(s);
  hearthEvent_t got = HEARTH_LINK_DOWN;
  int seen = 0;
  Hearth.onLinkEvent([&](hearthEvent_t e) { got = e; seen++; });
  s.injectURC("+MTREADY");
  Hearth.poll();
  check("an unexpected +MTREADY is a reboot", seen == 1 && got == HEARTH_COPROCESSOR_REBOOTED);
}

static void test_link_up_probe(void) {
  MockStream s;
  s.expect("AT", "OK\r\n");
  Hearth.begin(s);
  check("linkUp probes with a bare AT", Hearth.linkUp());
  check("script drained", s.scriptDrained());
}

int main(void) {
  printf("\n===== Hearth link tests =====\n");
  test_version();
  test_last_error_cleared_on_success();
  test_reboot_urc_raises_event();
  test_link_up_probe();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C test/host run`
Expected: `firmwareVersion`, `onLinkEvent`, `linkUp` undefined.

- [ ] **Step 3: Implement `HearthClass` in `src/Hearth.h` / `src/Hearth.cpp`**

Install a URC handler on the `HearthLink` that classifies `+MTREADY` and
dispatches `+MTEVT`, `+MTATTR` and `+MTIDENT` to the Matter layer (Task 5 fills
those three in; here, route `+MTREADY` only).

`hearthCommand()` stores the return of `HearthLink::command()`: a positive
value becomes `lastError()`, and `0` clears it.

- [ ] **Step 4: Run and confirm it passes**

Run: `make -C test/host run`
Expected: `RESULT: 6 passed, 0 failed` for this binary.

- [ ] **Step 5: Commit**

```bash
git add src/Hearth.h src/Hearth.cpp test/host/test_hearth.cpp test/host/Makefile
git commit -m "feat: the Hearth global for link config, diagnostics and events

Everything with no arduino-esp32 counterpart lives here rather than on the
Matter global, per naming rule N2: the Matter-named surface stays exactly what
upstream defines. This diverges from iLabs_ESP-NOW, which puts setLink() on its
parity global; ESP_NOW carries no trademark and Matter does.

An unexpected +MTREADY means the co-processor rebooted under us, which a host
needs to know because every endpoint ID it cached is now unconfirmed."
```

---

### Task 5: ArduinoMatter, the Matter global, and the reconcile flow

The core of the library. Implements design spec §5.4 verbatim.

**Files:**
- Modify: `src/Hearth.h`, `src/Hearth.cpp`
- Test: `test/host/test_reconcile.cpp`

**Interfaces:**
- Consumes: `HearthClass` (Task 4), `MatterEndPoint` registry (Task 3).
- Produces:
  ```cpp
  enum matterEvent_t { /* upstream names AND values, see Step 1 */ };

  class ArduinoMatter {
  public:
    using matterEventCB = std::function<void(matterEvent_t, const chip::DeviceLayer::ChipDeviceEvent *)>;
    static void onEvent(matterEventCB cb);
    static void begin();
    static String getManualPairingCode();
    static String getOnboardingQRCodeUrl();
    static bool isWiFiStationEnabled();
    static bool isWiFiAccessPointEnabled();
    static bool isThreadEnabled();
    static bool isBLECommissioningEnabled();
    static bool isDeviceCommissioned();
    static bool isWiFiConnected();
    static bool isThreadConnected();
    static bool isDeviceConnected();
    static void decommission();
  };
  extern ArduinoMatter Matter;
  ```

**Command mapping:**

| Method | Command | Notes |
|---|---|---|
| `begin()` | `AT+MTEP?` then §5.4 | see steps below |
| `isDeviceCommissioned()` | `AT+MTFABRICS?` | true when count > 0 |
| `getManualPairingCode()` | `AT+MTCODES?` | second field |
| `getOnboardingQRCodeUrl()` | `AT+MTCODES?` | first field, wrapped in the qrcode.html URL exactly as upstream does |
| `isWiFiConnected()` | `AT+MTNET?` | transport `WIFI` and connected `1` |
| `isThreadConnected()` | `AT+MTNET?` | transport `THREAD` and connected `1` |
| `isWiFiStationEnabled()` | `AT+MTNET?` | transport `WIFI` and enabled `1` |
| `isThreadEnabled()` | `AT+MTNET?` | transport `THREAD` and enabled `1` |
| `isDeviceConnected()` | `AT+MTNET?` | connected `1`, either transport |
| `isWiFiAccessPointEnabled()` | none | always `false`; the C6 image runs no SoftAP |
| `isBLECommissioningEnabled()` | none | always `true`; BLE commissioning is how the image commissions |
| `decommission()` | `AT+MTRESET` | reboots the C6 |

- [ ] **Step 1: Derive `matterEvent_t` from the SDK, do not transcribe it**

Upstream's enumerators take their values from CHIP's `DeviceEventType`. Read
them from
`~/esp/esp-matter/connectedhomeip/connectedhomeip/src/include/platform/CHIPDeviceEvent.h`
and from
`~/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Matter/src/Matter.h:56-160`,
and reproduce **both the names and the values**. The design spec requires the
values to match, and CLAUDE.md's rule about never transcribing IDs applies here
for the same reason.

- [ ] **Step 2: Write the `+MTEVT` bit to `matterEvent_t` mapping**

From `docs/AT_MT_SPEC.md` §3.11. Bits 10, 11 and 24 carry a detail of 1 or 0
for up and down, which goes into the stub `ChipDeviceEvent`.

```cpp
static const matterEvent_t kEventForBit[27] = {
  /*  0 */ MATTER_COMMISSIONING_WINDOW_OPEN,
  /*  1 */ MATTER_COMMISSIONING_SESSION_STARTED,
  /*  2 */ MATTER_COMMISSIONING_SESSION_STOPPED,
  /*  3 */ MATTER_COMMISSIONING_COMPLETE,
  /*  4 */ MATTER_COMMISSIONING_WINDOW_CLOSED,
  /*  5 */ MATTER_FAIL_SAFE_TIMER_EXPIRED,
  /*  6 */ MATTER_FABRIC_WILL_BE_REMOVED,
  /*  7 */ MATTER_FABRIC_REMOVED,
  /*  8 */ MATTER_FABRIC_COMMITTED,
  /*  9 */ MATTER_FABRIC_UPDATED,
  /* 10 */ MATTER_WIFI_CONNECTIVITY_CHANGE,
  /* 11 */ MATTER_INTERNET_CONNECTIVITY_CHANGE,
  /* 12 */ MATTER_INTERFACE_IP_ADDRESS_CHANGED,
  /* 13 */ MATTER_OPERATIONAL_NETWORK_STARTED,
  /* 14 */ MATTER_DNSSD_INITIALIZED,
  /* 15 */ MATTER_SERVER_READY,
  /* 16 */ MATTER_CHIPOBLE_CONNECTION_ESTABLISHED,
  /* 17 */ MATTER_CHIPOBLE_CONNECTION_CLOSED,
  /* 18 */ MATTER_CHIPOBLE_ADVERTISING_CHANGE,
  /* 19 */ MATTER_BLE_DEINITIALIZED,
  /* 20 */ MATTER_OTA_STATE_CHANGED,
  /* 21 */ MATTER_BINDINGS_CHANGED_VIA_CLUSTER,
  /* 22 */ MATTER_TIME_SYNC_CHANGE,
  /* 23 */ MATTER_ESP32_SPECIFIC_EVENT,          /* bit 23 is reserved */
  /* 24 */ MATTER_THREAD_CONNECTIVITY_CHANGE,
  /* 25 */ MATTER_THREAD_STATE_CHANGE,
  /* 26 */ MATTER_THREAD_INTERFACE_STATE_CHANGE,
};
```

- [ ] **Step 3: Write the failing test, `test/host/test_reconcile.cpp`**

```cpp
#include <stdio.h>
#include "ArduinoShim.h"
#include "MockStream.h"
#include "Hearth.h"
#include "MatterEndPoint.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

class TestEndPoint : public MatterEndPoint {
public:
  bool attributeChangeCB(uint16_t, uint32_t, uint32_t, esp_matter_attr_val_t *) override {
    return true;
  }
};

/* Steady state: what the sketch declares is already on the C6. One query, no
 * writes, no reboot. This is every boot after the first. */
static void test_identical_composition_adopts_ids(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint light, sensor;
  MatterEndPoint::hearthDeclare(&light, 0x0100);
  MatterEndPoint::hearthDeclare(&sensor, 0x0302);

  MockStream s;
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\n+MTEP:1,2,0x0302\r\nOK\r\n");
  Hearth.begin(s);
  Matter.begin();

  check("no further commands issued", s.scriptDrained());
  check("first endpoint adopts ID 1", light.getEndPointId() == 1);
  check("second endpoint adopts ID 2", sensor.getEndPointId() == 2);
}

/* First boot: the C6 has nothing, so the sketch's declaration is applied. */
static void test_empty_composition_applies(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint light;
  MatterEndPoint::hearthDeclare(&light, 0x0100);

  MockStream s;
  s.expect("AT+MTEP?", "OK\r\n");
  s.expect("AT+MTFABRICS?", "+MTFABRICS:0\r\nOK\r\n");
  s.expect("AT+MTEPCLEAR", "OK\r\n");
  s.expect("AT+MTEP=0x0100", "OK\r\n");
  s.expect("AT+MTEPAPPLY", "OK\r\n");
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\nOK\r\n");
  Hearth.begin(s);
  s.injectURC("+MTREADY");        /* the reboot the apply triggers */
  Matter.begin();

  check("full apply sequence issued", s.scriptDrained());
  check("endpoint adopts the ID from the re-query", light.getEndPointId() == 1);
}

/* Order is part of the composition: same types, different sequence, must
 * re-apply. This is what makes endpoint IDs reproducible per spec 5.3. */
static void test_reordered_composition_applies(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint sensor, light;
  MatterEndPoint::hearthDeclare(&sensor, 0x0302);
  MatterEndPoint::hearthDeclare(&light, 0x0100);

  MockStream s;
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\n+MTEP:1,2,0x0302\r\nOK\r\n");
  s.expect("AT+MTFABRICS?", "+MTFABRICS:0\r\nOK\r\n");
  s.expect("AT+MTEPCLEAR", "OK\r\n");
  s.expect("AT+MTEP=0x0302", "OK\r\n");
  s.expect("AT+MTEP=0x0100", "OK\r\n");
  s.expect("AT+MTEPAPPLY", "OK\r\n");
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0302\r\n+MTEP:1,2,0x0100\r\nOK\r\n");
  Hearth.begin(s);
  s.injectURC("+MTREADY");
  Matter.begin();

  check("reorder triggers a re-apply", s.scriptDrained());
  check("sensor is now endpoint 1", sensor.getEndPointId() == 1);
}

/* Spec 5.4: applying over a live fabric is allowed but never silent. */
static void test_commissioned_change_warns(void) {
  MatterEndPoint::hearthClearDeclarations();
  TestEndPoint light;
  MatterEndPoint::hearthDeclare(&light, 0x0101);

  MockStream s;
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\nOK\r\n");
  s.expect("AT+MTFABRICS?", "+MTFABRICS:1\r\nOK\r\n");
  s.expect("AT+MTEPCLEAR", "OK\r\n");
  s.expect("AT+MTEP=0x0101", "OK\r\n");
  s.expect("AT+MTEPAPPLY", "OK\r\n");
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0101\r\nOK\r\n");
  Hearth.begin(s);
  s.injectURC("+MTREADY");
  Matter.begin();

  check("it applies anyway, the sketch is the declaration of intent", s.scriptDrained());
  /* The flag lives on Hearth, not on Matter: rule N2 forbids adding anything
   * to a Matter-named class, including test introspection. */
  check("and it warned first", Hearth.warnedAboutRecommission());
}

static void test_event_urc_maps_to_enum(void) {
  MockStream s;
  Hearth.begin(s);
  matterEvent_t got = MATTER_SERVER_READY;
  int seen = 0;
  Matter.onEvent([&](matterEvent_t e, const chip::DeviceLayer::ChipDeviceEvent *) {
    got = e; seen++;
  });
  s.injectURC("+MTEVT:3");
  Hearth.poll();
  check("bit 3 is commissioning complete", seen == 1 && got == MATTER_COMMISSIONING_COMPLETE);
}

static void test_pairing_code(void) {
  MockStream s;
  s.expect("AT+MTCODES?", "+MTCODES:MT:Y.K9042C00KA0648G00,34970112332\r\nOK\r\n");
  Hearth.begin(s);
  check("manual code parsed", Matter.getManualPairingCode() == String("34970112332"));
}

static void test_commissioned_query(void) {
  MockStream s;
  s.expect("AT+MTFABRICS?", "+MTFABRICS:0\r\nOK\r\n");
  s.expect("AT+MTFABRICS?", "+MTFABRICS:2\r\nOK\r\n");
  Hearth.begin(s);
  check("zero fabrics is uncommissioned", !Matter.isDeviceCommissioned());
  check("two fabrics is commissioned", Matter.isDeviceCommissioned());
}

/* Endpoints registering after Matter.begin() are outside the reconciled
 * composition. Upstream's own examples call Matter.begin() last; this makes
 * getting it wrong loud rather than silent. */
static void test_late_declaration_is_reported(void) {
  MatterEndPoint::hearthClearDeclarations();
  MockStream s;
  s.expect("AT+MTEP?", "OK\r\n");
  Hearth.begin(s);
  Matter.begin();
  TestEndPoint late;
  check("a late declaration is refused", !MatterEndPoint::hearthDeclare(&late, 0x0100));
  check("and is reported as a protocol error", Hearth.lastError() != 0);
}

int main(void) {
  printf("\n===== reconcile and ArduinoMatter tests =====\n");
  test_identical_composition_adopts_ids();
  test_empty_composition_applies();
  test_reordered_composition_applies();
  test_commissioned_change_warns();
  test_event_urc_maps_to_enum();
  test_pairing_code();
  test_commissioned_query();
  test_late_declaration_is_reported();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Run and confirm it fails**

Run: `make -C test/host run`
Expected: `Matter` undefined.

- [ ] **Step 5: Implement `ArduinoMatter::begin()` as spec §5.4**

```
1. AT+MTEP?, collecting (index, endpoint_id, devtype) triples.
2. Compare the returned devtype sequence against hearthDeclaredTypeAt(0..n).
3. Identical: setEndPointId() on each declared endpoint from the listing,
   mark started, return. One query, zero writes.
4. Different: AT+MTFABRICS?; if non-zero, print the re-commissioning warning
   on Serial and record it. Then AT+MTEPCLEAR, one AT+MTEP=0x%04lX per declared
   endpoint in order, AT+MTEPAPPLY, wait up to 15000 ms for +MTREADY, re-query
   and continue at step 3.
```

The 15 s `+MTREADY` timeout is from spec §5.4. On timeout, raise
`HEARTH_PROTOCOL_ERROR` and leave endpoint IDs at 0 so later attribute writes
fail loudly rather than addressing endpoint 0, which the firmware does not
report on (§4).

- [ ] **Step 6: Route the three Matter URCs**

Extend the handler installed in Task 4:
- `+MTEVT:<bit>[,<detail>]` to the `matterEventCB` through `kEventForBit`.
- `+MTATTR:<ep>,<cl>,<attr>,<val>` to
  `hearthFindByEndPointId(ep)->attributeChangeCB(...)`, with the value rebuilt
  by `hearthAttrValFromLong` using the type the receiving class expects.
- `+MTIDENT:<ep>,<enabled>` to `hearthFindByEndPointId(ep)->endpointIdentifyCB(...)`.

An `ep` with no registered endpoint is dropped silently: the root endpoint and
any endpoint the sketch did not declare are legitimately not ours.

- [ ] **Step 7: Run and confirm it passes**

Run: `make -C test/host run`
Expected: `RESULT: 15 passed, 0 failed` for this binary.

- [ ] **Step 8: Commit**

```bash
git add src/Hearth.h src/Hearth.cpp test/host/test_reconcile.cpp test/host/Makefile
git commit -m "feat: ArduinoMatter, the Matter global, and the composition reconcile

Implements design spec 5.4. The steady state is one AT+MTEP? and zero writes:
every boot after the first, the sketch's declaration already matches what the
C6 persisted, so the library just adopts the endpoint IDs.

Reordering the same device types re-applies, because order is what makes
endpoint IDs reproducible across boots (spec 5.3), and IDs are what persisted
attribute values and the controller's cached PartsList are keyed on.

matterEvent_t values are derived from CHIPDeviceEvent.h rather than
transcribed, for the reason CLAUDE.md gives about device type IDs."
```

---

### Task 6: MatterOnOffLight

**Files:**
- Create: `src/MatterEndpoints/MatterOnOffLight.h`, `.cpp`
- Test: `test/host/test_onofflight.cpp`

**Interfaces:**
- Consumes: `MatterEndPoint` (Task 3), `Hearth` (Task 4).
- Produces: the class exactly as at
  `~/.arduino15/.../libraries/Matter/src/MatterEndpoints/MatterOnOffLight.h`.
  Reproduce that header's public section verbatim: `begin(bool initialState = false)`,
  `end()`, `setOnOff`, `getOnOff`, `toggle`, `onChange`, `onChangeOnOff`,
  `updateAccessory`, `operator bool`, `operator=(bool)`, `attributeChangeCB`,
  and the protected `started` / `onOffState` / `_onChangeCB` / `_onChangeOnOffCB`.

`begin()` calls `hearthDeclare(this, 0x0100)` and caches `initialState`; it
issues no AT traffic, because the endpoint ID is not known until
`Matter.begin()` reconciles. `getOnOff()` returns the cached state rather than
querying, matching upstream, which reads its local data model.

- [ ] **Step 1: Write the failing test, `test/host/test_onofflight.cpp`**

```cpp
#include <stdio.h>
#include "ArduinoShim.h"
#include "MockStream.h"
#include "Hearth.h"
#include "MatterEndpoints/MatterOnOffLight.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  cond ? g_pass++ : g_fail++;
}

static void bringUp(MockStream &s, MatterOnOffLight &light, bool initial) {
  MatterEndPoint::hearthClearDeclarations();
  Hearth.begin(s);
  light.begin(initial);
  s.expect("AT+MTEP?", "+MTEP:0,1,0x0100\r\nOK\r\n");
  Matter.begin();
}

static void test_begin_declares_and_adopts(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  check("declared as on_off_light", MatterEndPoint::hearthDeclaredTypeAt(0) == 0x0100);
  check("adopted endpoint 1", light.getEndPointId() == 1);
  check("begin() itself issued no AT traffic", s.scriptDrained());
}

static void test_set_on_off_writes(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  s.expect("AT+MTATTR=1,6,0,1,1", "OK\r\n");
  check("setOnOff(true) succeeds", light.setOnOff(true));
  check("state cached", light.getOnOff() == true);
  check("operator bool agrees", (bool)light == true);
}

static void test_toggle(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  s.expect("AT+MTATTR=1,6,0,1,1", "OK\r\n");
  check("toggle from off turns on", light.toggle() && light.getOnOff());
}

static void test_assignment_operator(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  s.expect("AT+MTATTR=1,6,0,1,1", "OK\r\n");
  light = true;
  check("operator= writes and caches", light.getOnOff() == true);
}

/* A controller turning the light on arrives as a +MTATTR URC. This is the
 * path the whole library exists for. */
static void test_controller_change_fires_callback(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  int onOffSeen = 0, changeSeen = 0;
  bool state = false;
  light.onChangeOnOff([&](bool v) { onOffSeen++; state = v; return true; });
  light.onChange([&](bool v) { changeSeen++; (void)v; return true; });
  s.injectURC("+MTATTR:1,6,0,1");
  Hearth.poll();
  check("onChangeOnOff fired", onOffSeen == 1 && state == true);
  check("onChange also fired", changeSeen == 1);
  check("cached state updated", light.getOnOff() == true);
  check("no echo written back to the fabric", s.scriptDrained());
}

static void test_failed_write_returns_false(void) {
  MockStream s; MatterOnOffLight light;
  bringUp(s, light, false);
  s.expect("AT+MTATTR=1,6,0,1,1", "+MTERR:2\r\nERROR\r\n");
  check("a rejected write returns false", !light.setOnOff(true));
  check("and does not update the cache", light.getOnOff() == false);
}

int main(void) {
  printf("\n===== MatterOnOffLight tests =====\n");
  test_begin_declares_and_adopts();
  test_set_on_off_writes();
  test_toggle();
  test_assignment_operator();
  test_controller_change_fires_callback();
  test_failed_write_returns_false();
  printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C test/host run`
Expected: `MatterEndpoints/MatterOnOffLight.h: No such file or directory`.

- [ ] **Step 3: Implement the class**

`attributeChangeCB` for cluster `0x0006` attribute `0x0000` updates
`onOffState`, then calls `_onChangeOnOffCB` and `_onChangeCB`. It must **not**
write back: the change already came from the fabric, which is what mode 0
exists for and what the last assertion in `test_controller_change_fires_callback`
guards.

`updateAccessory()` calls `_onChangeCB(onOffState)` so the sketch can drive the
physical light from the cached state, matching upstream.

- [ ] **Step 4: Run and confirm it passes**

Run: `make -C test/host run`
Expected: `RESULT: 14 passed, 0 failed` for this binary.

- [ ] **Step 5: Commit**

```bash
git add src/MatterEndpoints/MatterOnOffLight.h src/MatterEndpoints/MatterOnOffLight.cpp test/host/test_onofflight.cpp test/host/Makefile
git commit -m "feat: MatterOnOffLight

begin() declares the device type and caches the initial state but issues no AT
traffic: the endpoint ID does not exist until Matter.begin() reconciles, which
is why upstream's examples call Matter.begin() last.

attributeChangeCB deliberately does not write back. The change arrived from the
fabric, and echoing it is the loop that AT+MTATTR mode 0 was added to prevent."
```

---

### Task 7: MatterDimmableLight

**Files:**
- Create: `src/MatterEndpoints/MatterDimmableLight.h`, `.cpp`
- Test: `test/host/test_dimmable.cpp`

**Interfaces:**
- Consumes: `MatterEndPoint` (Task 3).
- Produces: the class exactly as upstream's
  `MatterEndpoints/MatterDimmableLight.h`: `MAX_BRIGHTNESS = 255`,
  `begin(bool initialState = false, uint8_t brightness = 64)`, `end()`,
  `setOnOff`, `getOnOff`, `toggle`, `setBrightness`, `getBrightness`,
  `onChangeOnOff(std::function<bool(bool)>)`,
  `onChangeBrightness(std::function<bool(uint8_t)>)`,
  `onChange(std::function<bool(bool, uint8_t)>)`, `updateAccessory`,
  `operator bool`, `operator=(bool)`, `attributeChangeCB`.

Device type `0x0101`. Brightness is LevelControl cluster `0x0008`, attribute
`CurrentLevel` `0x0000`, a `uint8`.

Note the signature difference from Task 6 that a copy-paste will get wrong:
`MatterOnOffLight::onChange` takes `bool(bool)`, while
`MatterDimmableLight::onChange` takes `bool(bool, uint8_t)`.

- [ ] **Step 1: Write the failing test, `test/host/test_dimmable.cpp`**

Mirror `test_onofflight.cpp`, with `+MTEP:0,1,0x0101` in `bringUp` and these
cases:

```cpp
static void test_brightness_write(void) {
  MockStream s; MatterDimmableLight light;
  bringUp(s, light, true, 64);
  s.expect("AT+MTATTR=1,8,0,128,1", "OK\r\n");
  check("setBrightness writes CurrentLevel", light.setBrightness(128));
  check("brightness cached", light.getBrightness() == 128);
}

static void test_brightness_from_controller(void) {
  MockStream s; MatterDimmableLight light;
  bringUp(s, light, true, 64);
  int seen = 0; uint8_t level = 0; int bothSeen = 0;
  light.onChangeBrightness([&](uint8_t v) { seen++; level = v; return true; });
  light.onChange([&](bool, uint8_t) { bothSeen++; return true; });
  s.injectURC("+MTATTR:1,8,0,200");
  Hearth.poll();
  check("onChangeBrightness fired", seen == 1 && level == 200);
  check("onChange fired with both values", bothSeen == 1);
  check("no echo", s.scriptDrained());
}

static void test_max_brightness_constant(void) {
  check("MAX_BRIGHTNESS is 255", MatterDimmableLight::MAX_BRIGHTNESS == 255);
}
```

Plus the on/off, toggle, adopt and failed-write cases from Task 6, adapted.

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C test/host run`

- [ ] **Step 3: Implement the class**

- [ ] **Step 4: Run and confirm it passes**

Run: `make -C test/host run`

- [ ] **Step 5: Commit**

```bash
git add src/MatterEndpoints/MatterDimmableLight.h src/MatterEndpoints/MatterDimmableLight.cpp test/host/test_dimmable.cpp test/host/Makefile
git commit -m "feat: MatterDimmableLight

onChange takes bool(bool, uint8_t) here against bool(bool) on MatterOnOffLight.
The signature is part of the parity contract, so it is reproduced rather than
harmonised, and the test asserts it."
```

---

### Task 8: MatterColorTemperatureLight and MatterTemperatureSensor

Two classes, because the sensor is the first read-direction endpoint and is
worth landing beside a write-direction one for contrast.

**Files:**
- Create: `src/MatterEndpoints/MatterColorTemperatureLight.h`, `.cpp`
- Create: `src/MatterEndpoints/MatterTemperatureSensor.h`, `.cpp`
- Test: `test/host/test_colortemp_sensor.cpp`

**Interfaces:**
- Consumes: `MatterEndPoint` (Task 3).
- Produces, from upstream's headers verbatim:
  - `MatterColorTemperatureLight`: device type `0x010C`. `MAX_BRIGHTNESS = 255`,
    `MAX_COLOR_TEMPERATURE = 500`, `MIN_COLOR_TEMPERATURE = 100`,
    `begin(bool initialState = false, uint8_t brightness = 64, uint16_t colorTemperature = 370)`,
    the three setter/getter pairs, `onChangeOnOff(bool(bool))`,
    `onChangeBrightness(bool(uint8_t))`, `onChangeColorTemperature(bool(uint16_t))`,
    `onChange(bool(bool, uint8_t, uint16_t))`, `updateAccessory`, the two
    operators, `attributeChangeCB`. Colour temperature is ColorControl cluster
    `0x0300`, attribute `ColorTemperatureMireds` `0x0007`, a `uint16`.
  - `MatterTemperatureSensor`: device type `0x0302`. `begin(double temperature = 0.00)`
    delegating to `begin(int16_t)`, `end()`, `setTemperature(double)`,
    `getTemperature()`, `setRawTemperature(int16_t)`, `operator=(double)`,
    `operator double()`, `attributeChangeCB`. TemperatureMeasurement cluster
    `0x0402`, attribute `MeasuredValue` `0x0000`, an **int16 in hundredths of a
    degree Celsius**, which goes negative and is why Task 2 tests signedness.

- [ ] **Step 1: Write the failing test, `test/host/test_colortemp_sensor.cpp`**

```cpp
static void test_color_temperature_write(void) {
  MockStream s; MatterColorTemperatureLight light;
  bringUpCT(s, light);
  s.expect("AT+MTATTR=1,768,7,370,1", "OK\r\n");
  check("setColorTemperature writes mireds", light.setColorTemperature(370));
  check("cached", light.getColorTemperature() == 370);
}

static void test_color_temperature_bounds(void) {
  check("MIN is 100", MatterColorTemperatureLight::MIN_COLOR_TEMPERATURE == 100);
  check("MAX is 500", MatterColorTemperatureLight::MAX_COLOR_TEMPERATURE == 500);
}

/* The sensor is host-driven: the sketch pushes readings up to the fabric. */
static void test_sensor_push(void) {
  MockStream s; MatterTemperatureSensor sensor;
  bringUpSensor(s, sensor);
  s.expect("AT+MTATTR=1,1026,0,2350,1", "OK\r\n");
  check("23.50 C becomes MeasuredValue 2350", sensor.setTemperature(23.50));
  check("and reads back as 23.5", sensor.getTemperature() > 23.49 && sensor.getTemperature() < 23.51);
}

/* Below freezing is the case an unsigned codec silently corrupts. */
static void test_sensor_negative(void) {
  MockStream s; MatterTemperatureSensor sensor;
  bringUpSensor(s, sensor);
  s.expect("AT+MTATTR=1,1026,0,-1234,1", "OK\r\n");
  check("-12.34 C is sent as -1234", sensor.setTemperature(-12.34));
  check("and reads back negative", sensor.getTemperature() < -12.33 && sensor.getTemperature() > -12.35);
}

static void test_sensor_operators(void) {
  MockStream s; MatterTemperatureSensor sensor;
  bringUpSensor(s, sensor);
  s.expect("AT+MTATTR=1,1026,0,1000,1", "OK\r\n");
  sensor = 10.0;
  check("operator double reads back", (double)sensor > 9.99 && (double)sensor < 10.01);
}
```

`bringUpCT` and `bringUpSensor` follow Task 6's `bringUp`, with
`+MTEP:0,1,0x010C` and `+MTEP:0,1,0x0302` respectively. Note the AT wire uses
decimal cluster IDs here (`768` is `0x0300`, `1026` is `0x0402`); spec §3.8
accepts either, and decimal is what `snprintf("%lu")` produces.

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C test/host run`

- [ ] **Step 3: Implement both classes**

- [ ] **Step 4: Run and confirm it passes**

Run: `make -C test/host run`

- [ ] **Step 5: Commit**

```bash
git add src/MatterEndpoints/MatterColorTemperatureLight.* src/MatterEndpoints/MatterTemperatureSensor.* test/host/test_colortemp_sensor.cpp test/host/Makefile
git commit -m "feat: MatterColorTemperatureLight and MatterTemperatureSensor

The sensor is the first read-direction endpoint: the sketch pushes readings and
nothing arrives from the fabric. Its MeasuredValue is a signed int16 in
hundredths of a degree, so a sub-zero reading is asserted explicitly. An
unsigned-only path passes every other assertion in this file."
```

---

### Task 9: Examples, keywords, README, and the four-endpoint gate

The proof that parity holds: upstream's own example sketches, byte-identical.

**Files:**
- Create: `examples/MatterOnOffLight/MatterOnOffLight.ino` (copied)
- Create: `examples/MatterDimmableLight/MatterDimmableLight.ino` (copied)
- Create: `examples/MatterTemperatureSensor/MatterTemperatureSensor.ino` (copied)
- Create: `keywords.txt`
- Create: `README.md`

- [ ] **Step 1: Copy the three upstream examples unmodified**

```bash
U=~/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Matter/examples
L=~/Arduino/libraries/iLabs_Hearth/examples
for e in MatterOnOffLight MatterDimmableLight MatterTemperatureSensor; do
  mkdir -p "$L/$e" && cp "$U/$e/$e.ino" "$L/$e/"
done
```

- [ ] **Step 2: Diff them against upstream and confirm they are identical**

```bash
for e in MatterOnOffLight MatterDimmableLight MatterTemperatureSensor; do
  diff "$U/$e/$e.ino" "$L/$e/$e.ino" && echo "$e identical"
done
```

Expected: three `identical` lines and no diff output. **If any file needed an
edit to compile, parity is not met and that is a finding, not a fix.** Record it
in the README's limitations section and raise it before proceeding.

- [ ] **Step 3: Note what these sketches need that the RP2350 lacks**

The upstream examples `#include <WiFi.h>` and `<Preferences.h>` and call
`WiFi.begin()`. On arduino-pico both headers exist, so they compile; `WiFi` on
the Pico W is a different radio from the C6's. Document in the README that on a
Challenger the C6 owns the Matter network connection and the sketch's `WiFi`
calls are inert for Matter's purposes. Do not modify the sketches to hide this.

- [ ] **Step 4: Write `keywords.txt`**

Arduino's format is `NAME<TAB>KEYWORD1` for classes and `KEYWORD2` for methods.
Cover both globals and all six public classes.

- [ ] **Step 5: Write `README.md`**

Cover: what the library is, the Hearth-and-Matter split and why (link to the
naming spec), wiring and the default `Serial1` link, a minimal example, the
supported device types (the four), and a **Limitations** section listing:
`createSecondaryNetworkInterface` unimplemented, `AT+MTATTRX` opaque types
unimplemented, the sixteen remaining device types, and the esp-matter revision
drift from spec §6.3 as a known release blocker.

- [ ] **Step 6: Run the full host suite one more time**

Run: `make -C test/host run`
Expected: every binary green.

- [ ] **Step 7: Commit**

```bash
git add examples keywords.txt README.md
git commit -m "docs: upstream examples verbatim, keywords and README

The three examples are byte-identical copies of arduino-esp32's, which is the
only real test of the parity claim: if one needs an edit to compile, parity is
not met.

They call WiFi.begin(), which on a Challenger drives the Pico's own radio and
not the C6's. Documented rather than patched out, because patching them would
forfeit the byte-identical property that makes them evidence."
```

---

### Task 10: Retire `iLabs_Matter` from the firmware repo's docs

The only task touching `iLabs_AT_Hearth`. Consequence of naming spec §7.1.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-26-at-mt-full-api-design.md` (§4 diagram, §12.2 phase table)
- Modify: `CLAUDE.md` (State section, "C4 next" line)

- [ ] **Step 1: Find every occurrence**

```bash
cd /mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/iLabs_AT_Hearth
grep -rn 'iLabs_Matter' --include='*.md' .
```

- [ ] **Step 2: Replace each with `iLabs_Hearth`**

Read each hit in context first. §4's architecture diagram and §12.2's phase
table both name it; neither needs rewording beyond the name.

- [ ] **Step 3: Update `CLAUDE.md`'s State section**

Change the C4 line to name `iLabs_Hearth`, and mark C4's status from the work
actually completed in Tasks 1 to 9.

- [ ] **Step 4: Add the ATLink de-duplication follow-up**

In the design spec's out-of-scope section, record that `HearthLink` and
`iLabs_ESP-NOW`'s `ATLink` are now two copies of one line protocol, that
merging them is worthwhile, and that doing so requires retesting the ESP-NOW
host library against its two-board rig, which is why C4 did not.

- [ ] **Step 5: Confirm nothing is left**

```bash
grep -rn 'iLabs_Matter' --include='*.md' . && echo "STILL PRESENT" || echo "clean"
```

Expected: `clean`.

- [ ] **Step 6: Commit**

```bash
git add -u
git commit -m "docs: retire the iLabs_Matter name

Consequence of the C4 naming spec section 7.1. The host library is iLabs_Hearth;
iLabs_Matter would have been a repo and artifact named Matter, which CLAUDE.md
forbids and which the naming spec exists to resolve.

Also records the HearthLink and ATLink duplication as a follow-up: merging them
needs the ESP-NOW two-board rig, which is why C4 copied rather than shared."
```

---

### Task 11: Hardware verification on a Challenger

Nothing before this proves the library talks to a real C6. The host tests prove
the library speaks the protocol it was told about; this proves the firmware
speaks the same one.

**Prerequisites:** a Challenger RP2350 with a C6 flashed with Hearth firmware,
a Matter controller (`chip-tool` or a hub), and both UARTs available per
CLAUDE.md's debugging note. **GPIO16/17 is the AT link, GPIO2 is the console.
Keep them as separate streams**: merged, they interleave per character and every
URC assertion goes flaky.

- [ ] **Step 1: Confirm the default link**

Task 4 assumed `Serial1` at `115200`. Confirm against the Challenger's actual
RP2350-to-C6 wiring and the bridge sketch. **If it is not `Serial1`, change
`HEARTH_DEFAULT_SERIAL` and say so**; the zero-configuration promise in
`Matter.begin()` depends on it.

- [ ] **Step 2: Flash and run `MatterOnOffLight`**

Expected on the console: the reconcile issues `AT+MTEP?`, finds an empty or
differing composition, applies, waits for `+MTREADY`, re-queries, and adopts
endpoint 1.

- [ ] **Step 3: Power cycle and confirm the steady state**

Expected: exactly one `AT+MTEP?` and no writes. This is design spec §5.4 step 3
and the thing that makes the scheme worth having.

- [ ] **Step 4: Confirm endpoint ID stability across three cold boots**

Expected: endpoint 1 every time. Spec §5.3 calls this load-bearing and the
firmware was verified for it on 2026-07-27; this confirms the host adopts it
correctly rather than renumbering.

- [ ] **Step 5: Commission and drive from a controller**

Toggle the light from the controller. Expected: a `+MTATTR:1,6,0,<v>` URC, the
sketch's `onChangeOnOff` fires, and **nothing is written back**. Watch the AT
link for an echo; that loop is the failure mode mode 0 exists to prevent and it
will not show up in the host tests.

- [ ] **Step 6: Drive from the sketch**

Expected: `AT+MTATTR=1,6,0,<v>,1` and the controller observes the change.

- [ ] **Step 7: Record the result**

Update `CLAUDE.md`'s State section: C4 hardware-verified, with the date, the
same way C1 and C2 are recorded. If anything failed, record what and stop; a
partially verified C4 must not be described as done.

- [ ] **Step 8: Commit**

```bash
git commit -m "docs: C4 hardware-verified on <date>

<what was run, on what, and anything that differed from the plan's assumptions,
particularly the default Serial the link uses.>"
```

---

## Self-Review

**Spec coverage.** Naming spec §3 N1 to N6: N1 Task 1 (`library.properties`),
N2 Tasks 3 and 5 (nothing added to Matter-named classes) and Task 4 (extensions
on `Hearth`), N3 Task 3 (`hearth*` statics), N4 Task 1 (`Matter.h` shim), N5
Task 4, N6 Task 4 (default link) and Task 11 Step 1 (verify it). §4 Task 1,
§4.1 already done, §4.2 Task 1 (`architectures=rp2040`), §5 file layout matches
Tasks 1 to 8, §6.1 closed set Tasks 5 to 8, §6.2 open set Task 4, §7.1 Task 10,
§8 open items: default `Serial` closed by Task 11 Step 1; the esp-matter
revision drift is **not** closed by this plan and is documented as a limitation
in Task 9 Step 5, which matches spec §7.2 calling it a release blocker rather
than C4 work.

Design spec §5.4 reconcile is Task 5 Step 5, all four branches tested. §3.8
attribute modes are Task 3. §3.11 event bits are Task 5 Step 2. §4 URCs are
Task 5 Step 6. §5 error codes are Task 1 and surfaced by Task 4's `lastError`.
The four slice device types are Tasks 6, 7, 8.

**Gap accepted:** `AT+MTCOMMISSION`, `AT+MTEVT=` mask setting, `AT+MTFRESET`
and `AT+MTSTATE?` have no arduino-esp32 counterpart and no Hearth method in this
plan. They are reachable through `Hearth.hearthCommand()` and belong in a later
task once a sketch needs them. Noted rather than silently omitted.

**Placeholder scan:** clean. Tasks 7 and 8 reference Task 6's test structure by
describing the changes rather than repeating the file, which the plan's own rule
discourages; both list their differing expectations in full, and the shared
`bringUp` helper is specified in each.

**Type consistency:** `hearthDeclare/hearthDeclaredCount/hearthDeclaredTypeAt/
hearthDeclaredAt/hearthClearDeclarations/hearthFindByEndPointId` are used with
those exact names in Tasks 3, 5, 6, 7, 8. `hearthCommand`, `hearthSetError` and
`lastError` are consistent across Tasks 3, 4, 5. `hearthAttrValToLong` and
`hearthAttrValFromLong` are consistent across Tasks 2, 3, 5.

One fix made during this review: the re-commissioning warning flag was first
written as `Matter.hearthWarnedAboutRecommission()`. That adds a member to a
Matter-named class, which N2 forbids with no exception for test introspection.
It is now `Hearth.warnedAboutRecommission()`, and Task 4's `HearthClass` must
declare it. The rule caught a violation on its first real use, which is the
argument for having stated it as a closed set rather than a guideline.
