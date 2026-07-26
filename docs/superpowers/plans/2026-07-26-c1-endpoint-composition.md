# Phase C1: Endpoint Composition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the C6's hardcoded single-endpoint data model with one the host declares over `AT+MT`, persists in NVS, and the firmware rebuilds unaided at boot.

**Architecture:** A pure-C composition codec (host-unit-testable) sits under a thin NVS store. A C++ device type table maps Matter device type IDs to `esp_matter` create thunks behind a C-linkage boundary. `mt_at.c` gains a staging state machine driving three new commands. `app_main` stops creating an on/off light literally and instead loops over the stored composition.

**Tech Stack:** ESP-IDF v5.4.1, esp-matter release/v1.5 (v1.5.1, commit `21aa3d1`), ESP32-C6, C for the AT layer, C++ for anything touching esp_matter, `gcc` for host unit tests.

**Spec:** `docs/superpowers/specs/2026-07-26-at-mt-full-api-design.md`. Section references below (§5, §6, §10, §12.1) are to that document.

## Global Constraints

- Target is **esp32c6 only**. Build with `idf.py set-target esp32c6 && idf.py build`.
- ESP-IDF **v5.4.1**, not the ESP-NOW firmware's v5.5.4. esp-matter checkout at `/home/pontus/esp/esp-matter`.
- **No em dashes** in any file: prose, comments, commit messages. Use a colon, comma, parentheses, or a full stop.
- Existing C style: 4-space indent, `/* */` block comments, `snake_case`, brace on the same line for control flow and on its own line for functions.
- `mt_at.c` is **C**. Anything touching `esp_matter` or CHIP must live in a `.cpp` file and be reached through a C-linkage bridge declared in a header under `main/include/`.
- Maximum **16 endpoints** (`MT_COMP_MAX_ENDPOINTS`), a RAM-driven cap.
- Device type IDs are **read from esp_matter** via `<ns>::get_device_type_id()`, never transcribed as literals (§6.1).
- `+MTERR` codes used by this phase: `1` bad parameter, `6` unknown device type, `7` persistence failure, `9` not ready, `10` composition change rejected (§10). Retrofitting the existing handlers to the new codes is phase C2, not this plan.
- **Never `git push`.** Commit only.

---

### Task 1: P1 spike, endpoint ID stability

Blocking precondition from §12.1. A negative result changes the design, so nothing else in this plan starts until this passes. Requires physical hardware and the BOOTSEL button, so it needs a human at the bench.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-26-at-mt-full-api-design.md` (record the result in §12.1)

**Interfaces:**
- Consumes: nothing.
- Produces: a recorded verdict. Tasks 2 onward assume PASS.

- [ ] **Step 1: Add three endpoints to the current firmware, temporarily**

In `main/main.cpp`, immediately after the existing `on_off_light::create(...)` block at line 235, add two more endpoints so there are three in a known order:

```cpp
    /* P1 SPIKE ONLY - remove before committing anything else. */
    dimmable_light::config_t spike_dim_config;
    endpoint_t *spike_ep2 = dimmable_light::create(node, &spike_dim_config, ENDPOINT_FLAG_NONE, nullptr);
    temperature_sensor::config_t spike_temp_config;
    endpoint_t *spike_ep3 = temperature_sensor::create(node, &spike_temp_config, ENDPOINT_FLAG_NONE, nullptr);
    ESP_LOGI(TAG, "P1 SPIKE endpoint ids: %u %u %u",
             endpoint::get_id(endpoint), endpoint::get_id(spike_ep2), endpoint::get_id(spike_ep3));
```

- [ ] **Step 2: Build and flash**

```bash
cd /home/pontus/Data/src/git/iLabs_AT_Matter
idf.py build
python3 fw/flash.py            # board in BOOTSEL
```

- [ ] **Step 3: Record the IDs from the console UART**

The console is on C6 **GPIO2** at 115200 (the AT link on GPIO16/17 carries no log output). Attach a USB-TTL probe and record the `P1 SPIKE endpoint ids:` line.

Expected: `1 2 3`. Write down whatever it actually says.

- [ ] **Step 4: Power-cycle five times and compare**

Fully remove power between each cycle, not just a reset. Record the line each time.

PASS: the three IDs are identical on every boot.
FAIL: any renumbering, gap, or reordering. Stop and report; §5.3 and the whole NVS scheme need rethinking.

- [ ] **Step 5: Test partial-build behaviour**

Change the middle endpoint's create call to force a failure, by passing a null node:

```cpp
    endpoint_t *spike_ep2 = dimmable_light::create(nullptr, &spike_dim_config, ENDPOINT_FLAG_NONE, nullptr);
```

Rebuild, flash, and read the log. The question being answered: when endpoint 2 fails to create, does endpoint 3 still get ID 3, or does it shift down to 2?

Record the answer. If it shifts, Task 6 must abort the whole boot rebuild on any single failure rather than skipping the failed entry, because a shifted ID silently corrupts a commissioned device's data model.

- [ ] **Step 6: Revert the spike and record the result**

```bash
git checkout main/main.cpp
```

Add the findings to §12.1 of the spec, in the observation table and as a verdict line under P1. Include the actual IDs observed and the partial-build answer.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/specs/2026-07-26-at-mt-full-api-design.md
git commit -m "docs: record the P1 endpoint-ID stability spike result"
```

---

### Task 2: Composition codec (pure C, host-tested)

The only part of C1 with real logic worth unit testing, and it has no IDF dependency, so it gets a proper red/green cycle on the host.

**Files:**
- Create: `main/include/mt_composition.h`
- Create: `main/mt_composition.c`
- Create: `test/host/test_mt_composition.c`
- Create: `test/host/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `mt_composition_t` struct with fields `uint16_t count` and `uint32_t devtype[MT_COMP_MAX_ENDPOINTS]`
  - `int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len)` returns bytes written or `-1`
  - `int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out)` returns `0` or `-1`
  - `bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b)`
  - `#define MT_COMP_MAX_ENDPOINTS 16`
  - `#define MT_COMP_BLOB_MAX (2 + 4 * MT_COMP_MAX_ENDPOINTS)`

- [ ] **Step 1: Write the header**

Create `main/include/mt_composition.h`:

```c
/*
 * mt_composition.h - the endpoint composition: an ordered list of Matter
 * device type IDs describing which endpoints this device presents.
 *
 * Pure C with no IDF dependency, so it is unit-testable on the host (see
 * test/host). Persistence lives in mt_comp_store.h; this file is only the
 * in-memory type and its wire encoding.
 *
 * Encoding is explicitly little-endian so a blob written by one build is
 * readable by another: u16 count, then count * u32 device type IDs.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum endpoints in a composition (RAM-driven cap). */
#define MT_COMP_MAX_ENDPOINTS 16

/* Maximum encoded blob size: u16 count + up to 16 u32 device type IDs. */
#define MT_COMP_BLOB_MAX (2 + 4 * MT_COMP_MAX_ENDPOINTS)

typedef struct {
    uint16_t count;
    uint32_t devtype[MT_COMP_MAX_ENDPOINTS];
} mt_composition_t;

/*
 * Serialise comp into buf. Returns the number of bytes written, or -1 if
 * comp->count exceeds MT_COMP_MAX_ENDPOINTS or buf is too small.
 */
int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len);

/*
 * Deserialise len bytes of buf into out. Returns 0 on success, or -1 if the
 * buffer is truncated, declares more endpoints than MT_COMP_MAX_ENDPOINTS,
 * or carries trailing bytes beyond the declared count.
 */
int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out);

/* True when a and b have the same count and the same IDs in the same order. */
bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing tests**

Create `test/host/test_mt_composition.c`:

```c
/*
 * Host unit tests for the composition codec. No framework: the project has
 * no test dependency and this needs none. Build and run with:
 *     make -C test/host && ./test/host/test_mt_composition
 */

#include <stdio.h>
#include <string.h>

#include "mt_composition.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void test_roundtrip(void)
{
    mt_composition_t in = { .count = 3, .devtype = { 0x0100, 0x0101, 0x0302 } };
    uint8_t buf[MT_COMP_BLOB_MAX];

    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("encode of 3 endpoints writes 14 bytes", n == 14);

    mt_composition_t out;
    memset(&out, 0xAA, sizeof(out));
    check("decode succeeds", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("roundtrip preserves the composition", mt_comp_equal(&in, &out));
}

static void test_empty(void)
{
    mt_composition_t in = { .count = 0 };
    uint8_t buf[MT_COMP_BLOB_MAX];

    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("empty composition encodes to 2 bytes", n == 2);

    mt_composition_t out = { .count = 99 };
    check("empty decodes", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("empty decodes to count 0", out.count == 0);
}

static void test_endianness(void)
{
    /* Pinned byte-for-byte: a blob written by one build must read on another. */
    mt_composition_t in = { .count = 1, .devtype = { 0x010C } };
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&in, buf, sizeof(buf));

    const uint8_t expect[6] = { 0x01, 0x00, 0x0C, 0x01, 0x00, 0x00 };
    check("encoding is little-endian and stable", n == 6 && memcmp(buf, expect, 6) == 0);
}

static void test_full(void)
{
    mt_composition_t in = { .count = MT_COMP_MAX_ENDPOINTS };
    for (int i = 0; i < MT_COMP_MAX_ENDPOINTS; i++) {
        in.devtype[i] = 0x0100u + (uint32_t)i;
    }
    uint8_t buf[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(&in, buf, sizeof(buf));
    check("a full composition encodes", n == MT_COMP_BLOB_MAX);

    mt_composition_t out;
    check("a full composition decodes", mt_comp_decode(buf, (size_t)n, &out) == 0);
    check("a full composition roundtrips", mt_comp_equal(&in, &out));
}

static void test_encode_rejects(void)
{
    mt_composition_t too_many = { .count = MT_COMP_MAX_ENDPOINTS + 1 };
    uint8_t buf[MT_COMP_BLOB_MAX];
    check("encode rejects an over-long composition",
          mt_comp_encode(&too_many, buf, sizeof(buf)) == -1);

    mt_composition_t ok = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    check("encode rejects a short buffer", mt_comp_encode(&ok, buf, 5) == -1);
}

static void test_decode_rejects(void)
{
    mt_composition_t out;

    check("decode rejects a zero-length buffer", mt_comp_decode(NULL, 0, &out) == -1);

    /* Declares 2 endpoints but carries only 1. */
    const uint8_t truncated[6] = { 0x02, 0x00, 0x00, 0x01, 0x00, 0x00 };
    check("decode rejects a truncated blob", mt_comp_decode(truncated, 6, &out) == -1);

    /* Declares 1 endpoint but carries 2. */
    const uint8_t trailing[10] = { 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00 };
    check("decode rejects trailing bytes", mt_comp_decode(trailing, 10, &out) == -1);

    /* Declares more endpoints than the cap. */
    const uint8_t oversize[2] = { (uint8_t)(MT_COMP_MAX_ENDPOINTS + 1), 0x00 };
    check("decode rejects an over-long count", mt_comp_decode(oversize, 2, &out) == -1);
}

static void test_equal(void)
{
    mt_composition_t a = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t b = { .count = 2, .devtype = { 0x0100, 0x0101 } };
    mt_composition_t reordered = { .count = 2, .devtype = { 0x0101, 0x0100 } };
    mt_composition_t shorter = { .count = 1, .devtype = { 0x0100 } };

    check("equal compositions compare equal", mt_comp_equal(&a, &b));
    check("order matters", !mt_comp_equal(&a, &reordered));
    check("count matters", !mt_comp_equal(&a, &shorter));
}

int main(void)
{
    printf("\n===== mt_composition codec tests =====\n");
    test_roundtrip();
    test_empty();
    test_endianness();
    test_full();
    test_encode_rejects();
    test_decode_rejects();
    test_equal();
    printf("\n===== RESULT: %d passed, %d failed =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
```

Create `test/host/Makefile`:

```make
# Host unit tests for the pure-C parts of the AT+MT firmware. No IDF, no
# framework: plain gcc. Run with `make -C test/host run`.

CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../../main/include

TESTS = test_mt_composition

all: $(TESTS)

test_mt_composition: test_mt_composition.c ../../main/mt_composition.c
	$(CC) $(CFLAGS) -o $@ $^

run: all
	./test_mt_composition

clean:
	rm -f $(TESTS)

.PHONY: all run clean
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cd /home/pontus/Data/src/git/iLabs_AT_Matter
make -C test/host run
```

Expected: the compile fails, because `main/mt_composition.c` does not exist yet. That is the red state.

- [ ] **Step 4: Write the implementation**

Create `main/mt_composition.c`:

```c
/*
 * mt_composition.c - encode, decode and compare endpoint compositions.
 *
 * Deliberately free of IDF dependencies so the codec can be unit-tested on
 * the host (test/host). Persistence is mt_comp_store.c's job.
 */

#include <string.h>

#include "mt_composition.h"

int mt_comp_encode(const mt_composition_t *comp, uint8_t *buf, size_t buf_len)
{
    if (!comp || !buf || comp->count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    size_t need = 2u + 4u * (size_t)comp->count;
    if (buf_len < need) {
        return -1;
    }

    buf[0] = (uint8_t)(comp->count & 0xFF);
    buf[1] = (uint8_t)((comp->count >> 8) & 0xFF);

    for (uint16_t i = 0; i < comp->count; i++) {
        uint32_t v = comp->devtype[i];
        buf[2 + 4 * i + 0] = (uint8_t)(v & 0xFF);
        buf[2 + 4 * i + 1] = (uint8_t)((v >> 8) & 0xFF);
        buf[2 + 4 * i + 2] = (uint8_t)((v >> 16) & 0xFF);
        buf[2 + 4 * i + 3] = (uint8_t)((v >> 24) & 0xFF);
    }

    return (int)need;
}

int mt_comp_decode(const uint8_t *buf, size_t len, mt_composition_t *out)
{
    if (!buf || !out || len < 2u) {
        return -1;
    }

    uint16_t count = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    if (count > MT_COMP_MAX_ENDPOINTS) {
        return -1;
    }

    /* Exact length: a truncated blob and a blob with trailing bytes are both
     * corruption, and silently accepting either would build the wrong data
     * model on a commissioned device. */
    if (len != 2u + 4u * (size_t)count) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->count = count;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[2 + 4 * i];
        out->devtype[i] = (uint32_t)p[0]
                        | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16)
                        | ((uint32_t)p[3] << 24);
    }

    return 0;
}

bool mt_comp_equal(const mt_composition_t *a, const mt_composition_t *b)
{
    if (!a || !b || a->count != b->count) {
        return false;
    }
    for (uint16_t i = 0; i < a->count; i++) {
        if (a->devtype[i] != b->devtype[i]) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
make -C test/host run
```

Expected: `===== RESULT: 19 passed, 0 failed =====` and exit code 0.

- [ ] **Step 6: Commit**

```bash
git add main/include/mt_composition.h main/mt_composition.c test/host/
git commit -m "C1: endpoint composition codec with host unit tests"
```

---

### Task 3: NVS composition store

**Files:**
- Create: `main/include/mt_comp_store.h`
- Create: `main/mt_comp_store.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `mt_composition_t`, `mt_comp_encode`, `mt_comp_decode`, `MT_COMP_BLOB_MAX` from Task 2.
- Produces:
  - `int mt_comp_store_load(mt_composition_t *out)` returns `0` loaded, `1` nothing stored, `-1` error
  - `int mt_comp_store_save(const mt_composition_t *comp)` returns `0` or `-1`

- [ ] **Step 1: Write the header**

Create `main/include/mt_comp_store.h`:

```c
/*
 * mt_comp_store.h - NVS persistence for the endpoint composition.
 *
 * A thin wrapper over the mt_composition codec. Kept separate so the codec
 * stays host-testable and this file stays small enough to read in one go.
 *
 * The composition lives in its own NVS namespace, distinct from the platform
 * namespace holding fabrics and attribute persistence, so clearing one does
 * not disturb the other.
 */

#pragma once

#include "mt_composition.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the stored composition into out.
 * Returns  0 on success,
 *          1 when nothing is stored (a factory-fresh device),
 *         -1 on an NVS or decode failure.
 */
int mt_comp_store_load(mt_composition_t *out);

/* Persist comp. Returns 0 on success, -1 on an encode or NVS failure. */
int mt_comp_store_save(const mt_composition_t *comp);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the implementation**

Create `main/mt_comp_store.c`:

```c
/*
 * mt_comp_store.c - read and write the endpoint composition in NVS.
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mt_comp_store.h"

static const char *TAG = "mt_comp_store";

#define MT_COMP_NVS_NAMESPACE "mt_ep"
#define MT_COMP_NVS_KEY       "comp"

int mt_comp_store_load(mt_composition_t *out)
{
    if (!out) {
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 1; /* namespace never written: factory fresh */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, MT_COMP_NVS_KEY, blob, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return -1;
    }

    if (mt_comp_decode(blob, len, out) != 0) {
        ESP_LOGE(TAG, "stored composition is corrupt (%u bytes)", (unsigned)len);
        return -1;
    }

    ESP_LOGI(TAG, "loaded composition: %u endpoint(s)", out->count);
    return 0;
}

int mt_comp_store_save(const mt_composition_t *comp)
{
    if (!comp) {
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(comp, blob, sizeof(blob));
    if (n < 0) {
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MT_COMP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_blob(h, MT_COMP_NVS_KEY, blob, (size_t)n);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving composition failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "saved composition: %u endpoint(s)", comp->count);
    return 0;
}
```

- [ ] **Step 3: Add both new sources to the build**

Modify `main/CMakeLists.txt`. Replace the `SRCS` block so it reads:

```cmake
    SRCS
        "main.cpp"
        "mt_at.c"
        "mt_composition.c"
        "mt_comp_store.c"
```

- [ ] **Step 4: Build**

```bash
cd /home/pontus/Data/src/git/iLabs_AT_Matter
idf.py build
```

Expected: `Project build complete.` with no warnings from the new files.

- [ ] **Step 5: Commit**

```bash
git add main/include/mt_comp_store.h main/mt_comp_store.c main/CMakeLists.txt
git commit -m "C1: persist the endpoint composition in its own NVS namespace"
```

---

### Task 4: Device type table

**Files:**
- Create: `main/include/mt_devtypes.h`
- Create: `main/mt_devtypes.cpp`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `bool mt_devtype_is_known(uint32_t devtype_id)`
  - `int mt_devtype_create(uint32_t devtype_id, uint16_t *out_ep_id)` returns `0` or `-1`

- [ ] **Step 1: Write the header**

Create `main/include/mt_devtypes.h`:

```c
/*
 * mt_devtypes.h - map a Matter device type ID to its esp_matter endpoint
 * constructor.
 *
 * C-linkage bridge: the table itself is C++ because every esp_matter endpoint
 * namespace is, but mt_at.c and the boot path reach it through plain C.
 *
 * Device type IDs are read from esp_matter's own get_device_type_id() rather
 * than transcribed, so the table cannot drift from the SDK we build against.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when devtype_id appears in the table. */
bool mt_devtype_is_known(uint32_t devtype_id);

/*
 * Create one endpoint of the given device type on the current node and write
 * its assigned endpoint ID to *out_ep_id.
 *
 * Must be called after node::create() and before esp_matter::start().
 * Returns 0 on success, -1 on an unknown device type or a creation failure.
 */
int mt_devtype_create(uint32_t devtype_id, uint16_t *out_ep_id);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the table**

Create `main/mt_devtypes.cpp`:

```cpp
/*
 * mt_devtypes.cpp - the device type table.
 *
 * One thunk per device type, because each esp_matter endpoint namespace has
 * its own config_t and the create() calls therefore do not share a signature.
 * Adding a device type is a thunk plus a row: mechanical, but not free.
 *
 * Verified against esp-matter release/v1.5 (v1.5.1). Namespace names differ
 * between esp-matter revisions, so check the header before adding a row: see
 * section 6.3 of the design spec.
 */

#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_log.h>

#include "mt_devtypes.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;

static const char *TAG = "mt_devtypes";

typedef endpoint_t *(*mt_devtype_ctor_t)(node_t *node);

typedef struct {
    uint32_t          id;
    mt_devtype_ctor_t create;
    const char       *name;
} mt_devtype_entry_t;

static endpoint_t *mk_on_off_light(node_t *n)
{
    on_off_light::config_t c;
    return on_off_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_dimmable_light(node_t *n)
{
    dimmable_light::config_t c;
    return dimmable_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_color_temperature_light(node_t *n)
{
    color_temperature_light::config_t c;
    return color_temperature_light::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

static endpoint_t *mk_temperature_sensor(node_t *n)
{
    temperature_sensor::config_t c;
    return temperature_sensor::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}

/* IDs come from esp_matter, never from a literal. */
static const mt_devtype_entry_t s_devtypes[] = {
    { on_off_light::get_device_type_id(),            mk_on_off_light,            "on_off_light"            },
    { dimmable_light::get_device_type_id(),          mk_dimmable_light,          "dimmable_light"          },
    { color_temperature_light::get_device_type_id(), mk_color_temperature_light, "color_temperature_light" },
    { temperature_sensor::get_device_type_id(),      mk_temperature_sensor,      "temperature_sensor"      },
};

static const size_t s_devtype_count = sizeof(s_devtypes) / sizeof(s_devtypes[0]);

static const mt_devtype_entry_t *find(uint32_t devtype_id)
{
    for (size_t i = 0; i < s_devtype_count; i++) {
        if (s_devtypes[i].id == devtype_id) {
            return &s_devtypes[i];
        }
    }
    return nullptr;
}

extern "C" bool mt_devtype_is_known(uint32_t devtype_id)
{
    return find(devtype_id) != nullptr;
}

extern "C" int mt_devtype_create(uint32_t devtype_id, uint16_t *out_ep_id)
{
    const mt_devtype_entry_t *e = find(devtype_id);
    if (!e || !out_ep_id) {
        ESP_LOGE(TAG, "unknown device type 0x%04X", (unsigned)devtype_id);
        return -1;
    }

    endpoint_t *ep = e->create(node::get());
    if (ep == nullptr) {
        ESP_LOGE(TAG, "creating %s (0x%04X) failed", e->name, (unsigned)devtype_id);
        return -1;
    }

    *out_ep_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "created %s (0x%04X) as endpoint %u", e->name, (unsigned)devtype_id, *out_ep_id);
    return 0;
}
```

- [ ] **Step 3: Add the source to the build**

Modify `main/CMakeLists.txt` so the `SRCS` block reads:

```cmake
    SRCS
        "main.cpp"
        "mt_at.c"
        "mt_composition.c"
        "mt_comp_store.c"
        "mt_devtypes.cpp"
```

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: `Project build complete.`

If a namespace name fails to resolve, do not guess a replacement. Check
`/home/pontus/esp/esp-matter/components/esp_matter/data_model/esp_matter_endpoint.h`
for the name this revision uses and record the discrepancy in §6.3 of the spec.

- [ ] **Step 5: Commit**

```bash
git add main/include/mt_devtypes.h main/mt_devtypes.cpp main/CMakeLists.txt
git commit -m "C1: device type table with the four slice endpoint types"
```

---

### Task 5: AT+MTEP staging commands

**Files:**
- Modify: `main/mt_at.c`
- Modify: `main/include/mt_matter.h`
- Modify: `main/main.cpp`

**Interfaces:**
- Consumes: `mt_composition_t`, `mt_comp_equal` (Task 2); `mt_comp_store_save` (Task 3); `mt_devtype_is_known` (Task 4).
- Produces, added to `mt_matter.h` and implemented in `main.cpp`:
  - `uint16_t mt_matter_endpoint_count(void)`
  - `int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id)` returns `0` or `-1`
  - `void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id)`

- [ ] **Step 1: Add the live-composition accessors to the bridge header**

In `main/include/mt_matter.h`, replace the `mt_matter_endpoint_id` declaration and its comment:

```c
/* Endpoint id of the on/off light. */
uint16_t mt_matter_endpoint_id(void);
```

with:

```c
/* ---- live composition (built at boot from the stored composition) ------ */

/* Number of endpoints this device currently presents, excluding the Root
 * Node on endpoint 0. Zero means unconfigured (design spec section 5.5). */
uint16_t mt_matter_endpoint_count(void);

/*
 * Describe the index'th live endpoint in creation order. Writes its Matter
 * device type ID and assigned endpoint ID. Returns 0 on success, -1 when
 * index is out of range.
 */
int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id);

/* Record an endpoint in the live table as the boot rebuild creates it. */
void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id);
```

- [ ] **Step 2: Implement the accessors**

In `main/main.cpp`, replace the existing implementation:

```cpp
extern "C" uint16_t mt_matter_endpoint_id(void)
{
    return s_light_endpoint_id;
}
```

with:

```cpp
/* The live composition, filled in by the boot rebuild in app_main. */
static uint32_t s_live_devtype[MT_COMP_MAX_ENDPOINTS];
static uint16_t s_live_ep_id[MT_COMP_MAX_ENDPOINTS];
static uint16_t s_live_count = 0;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id)
{
    if (index >= s_live_count || !devtype || !ep_id) {
        return -1;
    }
    *devtype = s_live_devtype[index];
    *ep_id   = s_live_ep_id[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id)
{
    if (s_live_count >= MT_COMP_MAX_ENDPOINTS) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count]   = ep_id;
    s_live_count++;
}
```

Add the include near the other project includes at the top of `main.cpp`:

```cpp
#include "mt_composition.h"
```

- [ ] **Step 3: Retire the single-light static**

In `main/main.cpp`, `app_attribute_update_cb` currently filters on one endpoint:

```cpp
    if (type == attribute::POST_UPDATE && endpoint_id == s_light_endpoint_id) {
```

Replace that condition with a test that admits every endpoint we built, which is every endpoint except the Root Node:

```cpp
    /* Surface changes on any endpoint we built. Endpoint 0 (Root Node) stays
     * suppressed to keep boot-time init noise off the AT link. */
    if (type == attribute::POST_UPDATE && endpoint_id != 0) {
```

Then delete the `s_light_endpoint_id` declaration and every remaining reference to it. Build errors will point at each one.

- [ ] **Step 4: Add the new error codes to mt_at.c**

In `main/mt_at.c`, extend the code-space block. Replace:

```c
#define MT_ERR_UNSUPPORTED  8   /* unknown/unsupported command */
#define MT_ERR_GENERIC      100 /* plain ERROR, no +MTERR line  */
#define MT_R_ERROR          MT_ERR_GENERIC
```

with:

```c
#define MT_ERR_BAD_PARAM    1   /* bad parameter or out of range          */
#define MT_ERR_DEVTYPE      6   /* unknown or unsupported device type     */
#define MT_ERR_PERSIST      7   /* NVS persistence failure                */
#define MT_ERR_UNSUPPORTED  8   /* unknown/unsupported command            */
#define MT_ERR_NOT_READY    9   /* no composition, or stack not started   */
#define MT_ERR_COMP_REJECT  10  /* nothing staged, or endpoint cap hit    */
#define MT_ERR_GENERIC      100 /* plain ERROR, no +MTERR line            */
#define MT_R_ERROR          MT_ERR_GENERIC
```

- [ ] **Step 5: Add the includes and staging state to mt_at.c**

Add to the include block in `main/mt_at.c`, after `#include "mt_matter.h"`:

```c
#include "esp_system.h"
#include "mt_comp_store.h"
#include "mt_composition.h"
#include "mt_devtypes.h"
```

Add the staging state immediately above the `/* ---- dispatch table & registration ---- */` comment:

```c
/* ---- endpoint composition staging (C1) -------------------------------- *
 * AT+MTEPCLEAR opens a staging session, AT+MTEP= appends to it, and
 * AT+MTEPAPPLY persists it and reboots. Staging lives in RAM only, so an
 * interrupted host leaves the stored composition untouched rather than
 * half-written.                                                            */

static mt_composition_t s_staged;
static bool             s_staging = false;
```

- [ ] **Step 6: Write the three handlers**

Add to `main/mt_at.c`, immediately below the staging state:

```c
/* AT+MTEPCLEAR -> begin staging an empty composition. */
static int cmd_mtepclear(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    s_staged.count = 0;
    s_staging = true;
    return AT_R_OK;
}

/*
 * AT+MTEP?            -> +MTEP:<index>,<endpoint_id>,<device_type> per endpoint
 * AT+MTEP=<devtype>   -> append one endpoint to the staged composition
 *
 * The query always reports the LIVE composition, never the staged one.
 */
static int cmd_mtep(at_type_t type, char *args)
{
    if (type == AT_QUERY) {
        uint16_t n = mt_matter_endpoint_count();
        for (uint16_t i = 0; i < n; i++) {
            uint32_t devtype;
            uint16_t ep_id;
            if (mt_matter_endpoint_info(i, &devtype, &ep_id) == 0) {
                at_uart_write_line("+MTEP:%u,%u,0x%04lX", i, ep_id, (unsigned long)devtype);
            }
        }
        return AT_R_OK;
    }

    if (type != AT_SET) {
        return MT_R_ERROR;
    }
    if (!s_staging) {
        return MT_ERR_COMP_REJECT;
    }
    if (s_staged.count >= MT_COMP_MAX_ENDPOINTS) {
        return MT_ERR_COMP_REJECT;
    }

    unsigned long devtype;
    if (!parse_u(args, &devtype)) {
        return MT_ERR_BAD_PARAM;
    }
    if (!mt_devtype_is_known((uint32_t)devtype)) {
        return MT_ERR_DEVTYPE;
    }

    s_staged.devtype[s_staged.count++] = (uint32_t)devtype;
    return AT_R_OK;
}

/* AT+MTEPAPPLY -> persist the staged composition, then reboot. */
static int cmd_mtepapply(at_type_t type, char *args)
{
    (void)args;
    if (type != AT_EXEC) {
        return MT_R_ERROR;
    }
    if (!s_staging) {
        return MT_ERR_COMP_REJECT;
    }
    if (mt_comp_store_save(&s_staged) != 0) {
        return MT_ERR_PERSIST;
    }

    s_staging = false;

    /* Acknowledge, drain the UART, then reboot. The host resynchronizes on
     * the next "+MTREADY", exactly as it does after AT+MTRESET. */
    at_uart_write_line("OK");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return AT_R_DONE;
}
```

Note: `parse_u` already exists in `mt_at.c` (it backs `AT+MTATTR`) and accepts hex or decimal, so `AT+MTEP=0x0100` and `AT+MTEP=256` are both valid. It is defined above `cmd_mtattr`; the new handlers must appear after that definition. If the compiler reports an implicit declaration, move the staging block below `cmd_mtattr`.

- [ ] **Step 7: Register the commands**

In `main/mt_at.c`, extend `s_cmds` so it reads:

```c
static const at_command_t s_cmds[] = {
    { "CGMI",         cmd_cgmi        },
    { "CGMM",         cmd_cgmm        },
    { "CGMR",         cmd_cgmr        },
    { "MTVER",        cmd_ver         },
    { "MTSTATE",      cmd_mtstate     },
    { "MTFABRICS",    cmd_mtfabrics   },
    { "MTCOMMISSION", cmd_mtcommission },
    { "MTCODES",      cmd_mtcodes     },
    { "MTRESET",      cmd_mtreset     },
    { "MTATTR",       cmd_mtattr      },
    { "MTEP",         cmd_mtep        },
    { "MTEPCLEAR",    cmd_mtepclear   },
    { "MTEPAPPLY",    cmd_mtepapply   },
};
```

- [ ] **Step 8: Build**

```bash
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 9: Commit**

```bash
git add main/mt_at.c main/include/mt_matter.h main/main.cpp
git commit -m "C1: AT+MTEP staging commands and the live composition accessors"
```

---

### Task 6: Boot rebuild and the app_main restructure

The task that actually removes the hardcoded data model.

**Files:**
- Modify: `main/main.cpp:220-256`

**Interfaces:**
- Consumes: `mt_comp_store_load` (Task 3), `mt_devtype_create` (Task 4), `mt_matter_record_endpoint` (Task 5).
- Produces: `static void mt_boot_window_policy(bool configured)`, the P2 seam from §12.1.

- [ ] **Step 1: Add the includes**

At the top of `main/main.cpp`, alongside the existing project includes:

```cpp
#include "mt_comp_store.h"
#include "mt_devtypes.h"
```

- [ ] **Step 2: Add the P2 seam**

Immediately above `app_main` in `main/main.cpp`:

```cpp
/*
 * Boot commissioning-window policy. Isolated here because open question P2
 * (design spec section 12.1) is unresolved: an unconfigured device is
 * specified not to advertise, but CHIP auto-opens a window at boot when the
 * node is uncommissioned, and whether that is cleanly suppressible on this
 * esp-matter revision is not yet established.
 *
 * Until P2 concludes this only logs what it would do. Do not scatter window
 * decisions elsewhere: this function is the single place that changes when
 * P2 lands.
 */
static void mt_boot_window_policy(bool configured)
{
    if (configured) {
        ESP_LOGI(TAG, "boot window policy: configured device, default CHIP behaviour");
    } else {
        ESP_LOGW(TAG, "boot window policy: UNCONFIGURED, spec section 5.5 wants no "
                      "commissioning window here (open question P2)");
    }
}
```

- [ ] **Step 3: Replace the hardcoded endpoint creation**

In `main/main.cpp`, replace this block (currently lines 233 to 241):

```cpp
    /* One on/off-light endpoint - the simplest controllable device type. */
    on_off_light::config_t light_config;
    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "Failed to create on_off_light endpoint");
        return;
    }
    s_light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "on/off light endpoint created, id=%u", s_light_endpoint_id);
```

with the composition rebuild:

```cpp
    /*
     * Rebuild the endpoint composition the host declared over AT+MTEP. The
     * device does this unaided so it rejoins its fabric after a power cut
     * without waiting on the host (design spec section 5.3).
     */
    mt_composition_t comp;
    int rc = mt_comp_store_load(&comp);
    if (rc < 0) {
        ESP_LOGE(TAG, "composition load failed, starting unconfigured");
        comp.count = 0;
    } else if (rc == 1) {
        ESP_LOGI(TAG, "no stored composition, starting unconfigured");
        comp.count = 0;
    }

    for (uint16_t i = 0; i < comp.count; i++) {
        uint16_t ep_id = 0;
        if (mt_devtype_create(comp.devtype[i], &ep_id) != 0) {
            /*
             * Abort the whole rebuild rather than skipping the failed entry.
             * A partial build shifts the endpoint IDs of everything after it,
             * which silently corrupts a commissioned device's data model.
             */
            ESP_LOGE(TAG, "endpoint %u (0x%04X) failed, aborting rebuild",
                     i, (unsigned)comp.devtype[i]);
            comp.count = 0;
            break;
        }
        mt_matter_record_endpoint(comp.devtype[i], ep_id);
    }

    ESP_LOGI(TAG, "composition rebuilt: %u endpoint(s)", mt_matter_endpoint_count());
```

If Task 1 Step 5 established that a failed create does **not** shift subsequent IDs, this abort is stricter than necessary but still correct. Leave it: the cost is a rebuild that refuses to run half-configured, and the alternative risks silent corruption.

- [ ] **Step 4: Call the window policy after the stack starts**

In `main/main.cpp`, after the `esp_matter::start(app_event_cb)` error check and before `mt_at_start()`, replace this line:

```cpp
    ESP_LOGI(TAG, "Matter started, on/off light ready on endpoint %u", s_light_endpoint_id);
```

with:

```cpp
    mt_boot_window_policy(mt_matter_endpoint_count() > 0);
    ESP_LOGI(TAG, "Matter started with %u endpoint(s)", mt_matter_endpoint_count());
```

- [ ] **Step 5: Build**

```bash
idf.py build
```

Expected: `Project build complete.` with no reference to `s_light_endpoint_id` remaining anywhere.

```bash
grep -rn "s_light_endpoint_id" main/
```

Expected: no output.

- [ ] **Step 6: Flash and verify the unconfigured state**

```bash
python3 fw/flash.py            # board in BOOTSEL
```

Then open the AT link. Once `flash.py` finishes, the RP2350 bridge is still running, so `/dev/ttyACM0` is the AT link at 115200.

The C6 still holds a composition-free NVS namespace on first run after this change, so it must come up unconfigured:

```
+MTREADY

AT
OK

AT+MTEP?
OK
```

`AT+MTEP?` returning `OK` with no `+MTEP:` lines is the unconfigured state (§5.5). The console on GPIO2 should carry the `boot window policy: UNCONFIGURED` warning.

- [ ] **Step 7: Verify the staging round-trip**

```
AT+MTEP=0x0100
+MTERR:10
ERROR

AT+MTEPCLEAR
OK

AT+MTEP=0x0100
OK

AT+MTEP=0x0302
OK

AT+MTEP=0x9999
+MTERR:6
ERROR

AT+MTEPAPPLY
OK
```

The device then reboots and emits `+MTREADY`. Confirm the composition came back:

```
AT+MTEP?
+MTEP:0,1,0x0100
+MTEP:1,2,0x0302
OK
```

The first `AT+MTEP=` must be rejected with `+MTERR:10` because no staging session is open. `0x9999` must be rejected with `+MTERR:6` and must not consume a slot: the applied composition has exactly two endpoints.

- [ ] **Step 8: Verify persistence across a power cut**

Physically remove power, restore it, and re-run `AT+MTEP?`. The same two lines with the same endpoint IDs must come back, with no host involvement. This is the whole point of §5.3, so treat a mismatch here as a stop-and-report.

- [ ] **Step 9: Verify the attribute path still works**

```
AT+MTATTR=1,6,0
+MTATTR:1,6,0,0
OK

AT+MTATTR=1,6,0,1
OK

AT+MTATTR=1,6,0
+MTATTR:1,6,0,1
OK
```

Endpoint 1 is the on/off light from the composition. This confirms Task 5's change to `app_attribute_update_cb` did not break the URC path.

- [ ] **Step 10: Commit**

```bash
git add main/main.cpp
git commit -m "C1: rebuild the endpoint composition at boot from NVS"
```

---

### Task 7: Document the new commands

**Files:**
- Modify: `docs/AT_MT_SPEC.md`

**Interfaces:**
- Consumes: the command behaviour established in Tasks 5 and 6.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Add the three commands to the summary table in §2**

Add these rows after the `AT+MTATTR` rows:

```markdown
| `AT+MTEP?` | query | `+MTEP:<idx>,<ep_id>,<devtype>` per endpoint → `OK` |
| `AT+MTEP=<devtype>` | set | `OK` (append to the staged composition) |
| `AT+MTEPCLEAR` | exec | `OK` (begin staging an empty composition) |
| `AT+MTEPAPPLY` | exec | `OK` → persist + reboot |
```

- [ ] **Step 2: Add a reference section**

Add as §3.9, after the `AT+MTATTR` section:

```markdown
### 3.9 `AT+MTEP` / `AT+MTEPCLEAR` / `AT+MTEPAPPLY` - endpoint composition

The host declares which endpoints the device presents. The composition is
persisted in NVS and rebuilt by the firmware at every boot, so the device
rejoins its fabric after a power cut without host involvement.

- `AT+MTEP?` lists the **live** composition, one line per endpoint:
  `+MTEP:<index>,<endpoint_id>,<device_type>`. Zero lines means the device is
  unconfigured. It never reports a staged composition.
- `AT+MTEPCLEAR` opens a staging session holding an empty composition.
- `AT+MTEP=<device_type>` appends one endpoint. `<device_type>` is a standard
  Matter device type ID, hex or decimal. Rejected with `+MTERR:10` outside a
  staging session or past the 16-endpoint cap, and with `+MTERR:6` for a device
  type this firmware does not implement.
- `AT+MTEPAPPLY` persists the staged composition, emits `OK`, and reboots. The
  host resynchronizes on the next `+MTREADY`.

Staging lives in RAM, so a reboot discards an open session and leaves the
stored composition untouched.

An **unconfigured** device (no stored composition) presents only the Root Node
and rejects `AT+MTATTR` with `+MTERR:9`. Changing the composition of a
commissioned device invalidates controller caches and may require
re-commissioning.

Device types implemented so far: `0x0100` On/Off Light, `0x0101` Dimmable
Light, `0x010C` Colour Temperature Light, `0x0302` Temperature Sensor.
```

- [ ] **Step 3: Replace the error code table in §5**

Replace the two-row table with:

```markdown
| Code | Meaning |
|---|---|
| `1` | Bad parameter or out of range |
| `6` | Unknown or unsupported device type |
| `7` | Persistence (NVS) failure |
| `8` | Unknown / unsupported command (version-skew detection) |
| `9` | Not ready: no composition declared, or stack not started |
| `10` | Composition change rejected: nothing staged, or endpoint limit exceeded |
| (bare `ERROR`) | Bad parameters, wrong command form, or a runtime failure |

Codes `2` to `5` are allocated in the design spec and land in phase C2, when
the existing handlers are retrofitted to them.
```

- [ ] **Step 4: Update §7, the data model section**

Replace the "One endpoint: an on/off light on endpoint 1" bullet with:

```markdown
- Endpoints are **declared by the host** via `AT+MTEP` and persisted on the
  device (§3.9). Endpoint `0` is the mandatory Root Node. A factory-fresh
  device presents no application endpoints until the host declares them.
```

- [ ] **Step 5: Commit**

```bash
git add docs/AT_MT_SPEC.md
git commit -m "docs: specify AT+MTEP endpoint composition and the C1 error codes"
```

---

## Self-Review

**Spec coverage.** §5.1 commands map to Task 5 and Task 6 Step 7. §5.2 NVS format maps to Tasks 2 and 3. §5.3 boot sequence maps to Task 6. §5.4 host reconcile is C4, correctly out of this plan. §5.5 unconfigured state maps to Task 6 Steps 2, 4 and 6, with the window half behind the P2 seam. §6.1 and §6.2 map to Task 4. §10 codes used by C1 map to Task 5 Step 4; codes `2` to `5` are C2 and Task 7 Step 3 says so. §12.1 P1 maps to Task 1, P2 to Task 6 Step 2.

**Deliberately out of scope**, from the spec's C1 row: nothing. The `AT+MTATTR` mode parameter and the `+MTERR` retrofit are C2, events are C3.

**Type consistency.** `mt_composition_t` fields `count` and `devtype[]` are used identically in Tasks 2, 3, 5 and 6. `mt_comp_store_load` returns `0`/`1`/`-1` in Task 3 and is consumed with that exact contract in Task 6 Step 3. `mt_devtype_create(uint32_t, uint16_t *)` is declared in Task 4 and called with that signature in Task 6. `mt_matter_record_endpoint(uint32_t, uint16_t)` is declared in Task 5 Step 1, implemented in Step 2, and called in Task 6 Step 3. `MT_COMP_MAX_ENDPOINTS` is defined once in Task 2 and used in Tasks 5 and 6.

**Known gap, deliberate.** Task 5 Step 6 notes that `parse_u` is a `static` already present in `mt_at.c` and that the new handlers must sit after its definition. If the file is reorganised during implementation this could bite; the step says what to do about it.
