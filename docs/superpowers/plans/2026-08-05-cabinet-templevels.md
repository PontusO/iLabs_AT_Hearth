# Cabinet + Level Labels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the upstream class set (20 of 20) with
MatterTemperatureControlledCabinet: composition variants (v2 blob), the
fifth abort trap, the CHIP level-list delegate, quoted-string label
transport via AT+MTTEMPLEVELS, and the library class in both modes.

**Architecture:** Per the spec
(`docs/superpowers/specs/2026-08-05-cabinet-templevels-design.md`).
Variants are a generic composition mechanism; the cabinet consumes them
(0 = number, 1 = level). Labels live host-side and cross the wire as
quoted strings into a firmware delegate; nothing persists them on the
C6.

**Tech Stack:** Pure C codec (host-tested), C/C++ firmware, MockStream
library tests, esp-matter 21aa3d1 pinned.

## Global Constraints

- **No em dashes** anywhere. mt_at.c and mt_composition.c stay C with no
  SDK headers; bridges hold ChipStackLock; delegate registration happens
  once before esp_matter::start().
- The v2 blob discriminator: a v1 blob's first byte is a count (0..16),
  so v2 starts with sentinel 0xFF then version 0x02. Decoder accepts
  both; encoder always writes v2. Blob max becomes
  2 + 2 + 16 * 5 = 84 bytes.
- `AT+MTEP?` emits the variant field ONLY when nonzero (existing hosts
  see byte-identical output). Grammar: `AT+MTEP=<id>[,<variant>]`;
  invalid variant for the type answers +MTERR:1 at staging.
- AT+MTTEMPLEVELS error codes: +MTERR:1 grammar/label violations
  (count 1..16, label 1..16 printable ASCII bytes, no double quote in a
  label); +MTERR:2 unknown endpoint; +MTERR:3 no TemperatureControl
  cluster; +MTERR:4 cluster present but no SupportedTemperatureLevels
  (number-variant cabinet). Set-only; bare ERROR on wrong form.
- The fifth abort trap: temperature_control::create() runs
  VALIDATE_FEATURES_EXACT_ONE(TemperatureNumber, TemperatureLevel)
  (esp_matter_cluster.cpp:2849-2850); every cabinet thunk MUST set
  exactly one feature bit; trap comments follow the house convention.
- Every SDK name verified against the pinned headers before use, quoted
  in reports; upstream arduino-esp32's cabinet .cpp is API reference
  only (its bundled esp-matter differs structurally).
- All three firmware images green at firmware task boundaries;
  scripts/apply-sdk-patches.sh --check "applied" x2 around builds; host
  suites green in the touched repo at every boundary.
- Library work on branch `cabinet` in
  /home/pontus/Data/Dropbox/Arduino/libraries/iLabs_Hearth (Dropbox:
  in place, never symlink). Standing library rules: TDD with red
  excerpts; decimal IDs in expect(); no binaries in commits (.gitignore
  lines with Makefile lines); NEW commits never amends; parity surfaces
  untouched; 2-space /* */.
- Commit messages explain why and end exactly with:

```
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01A4hbCu9g2vEzuATC11ZsYc
```

---

### Task C1: Composition codec v2 (pure C, full TDD)

**Files:**
- Modify: `main/include/mt_composition.h`, `main/mt_composition.c`
- Test: `test/host/test_mt_composition.c`

**Interfaces:**
- Produces:

```c
#define MT_COMP_BLOB_V2_SENTINEL 0xFF
#define MT_COMP_BLOB_VERSION     0x02
#define MT_COMP_BLOB_MAX (2 + 2 + 5 * MT_COMP_MAX_ENDPOINTS)

typedef struct {
    uint16_t count;
    uint32_t devtype[MT_COMP_MAX_ENDPOINTS];
    uint8_t  variant[MT_COMP_MAX_ENDPOINTS];
} mt_composition_t;
```

  encode writes v2 always ([0xFF, 0x02, count LE u16, then per entry
  u32 id LE + u8 variant]); decode dispatches on byte0 (<= 16: v1,
  variants all 0; 0xFF: v2 with version check); equal compares
  variants too.

- [ ] **Step 1: tests first** (extend test_mt_composition.c following
its existing test-function style): v1 golden blob decodes with zero
variants; v2 round-trip with mixed variants; v1-vs-v2 equality when
variants are zero; decode rejects: bad version byte after 0xFF,
truncated v2, trailing bytes, count over max; encode length check;
endianness pin for the v2 layout (fixed byte sequence). Run red
(struct/API changes missing), then implement, then green
(`make -C test/host run`).
- [ ] **Step 2:** update the header comment block (the encoding doc at
mt_composition.h:9-11) for both formats; adjust MT_COMP_BLOB_MAX users
if any size the buffer from it (grep mt_comp_store.c and mt_at.c).
- [ ] **Step 3:** firmware still builds (build_b4 only at this stage:
the struct gained a field; callers compile because count/devtype are
unchanged and variant zero-fills via existing memset/initializer
patterns: verify how s_staged is initialized in mt_at.c and keep
variants zeroed there until C2 wires them). Commit.

### Task C2: Variant-aware staging, table and the cabinet thunks

**Files:**
- Modify: `main/mt_at.c` (cmd_mtep parses the optional variant;
  cmd_mtep query emits it when nonzero), `main/include/mt_devtypes.h`,
  `main/mt_devtypes.cpp`, `main/main.cpp` (boot rebuild passes variants)

**Interfaces:**
- Produces:

```c
bool mt_devtype_is_known(uint32_t devtype_id);
bool mt_devtype_variant_ok(uint32_t devtype_id, uint8_t variant);
int  mt_devtype_create(uint32_t devtype_id, uint8_t variant, uint16_t *out_ep_id);
```

  (is_known keeps its signature; variant_ok is new; create gains the
  variant argument: update its two call sites, boot rebuild and
  wherever EPAPPLY drives creation.)

- [ ] **Step 1:** mt_devtypes.cpp: the entry struct gains
`uint8_t max_variant;` (0 for every existing row); the thunk typedef
gains a variant parameter (all existing thunks ignore it); add the
cabinet row `{ temperature_controlled_cabinet::get_device_type_id(),
mk_temperature_controlled_cabinet, "temperature_controlled_cabinet",
1 }` with:

```cpp
static endpoint_t *mk_temperature_controlled_cabinet(node_t *n, uint8_t variant)
{
    temperature_controlled_cabinet::config_t c;
    /* Fifth abort trap: temperature_control::create() runs
     * VALIDATE_FEATURES_EXACT_ONE on TemperatureNumber/TemperatureLevel
     * (esp_matter_cluster.cpp:2849). Exactly one bit, chosen by the
     * composition variant: 0 = number, 1 = level. */
    if (variant == 1) {
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_level::get_id();
    } else {
        c.temperature_control.feature_flags =
            cluster::temperature_control::feature::temperature_number::get_id();
    }
    return temperature_controlled_cabinet::create(n, &c, ENDPOINT_FLAG_NONE, nullptr);
}
```

(Verify the endpoint namespace name, config member path and feature
namespaces in the pinned headers; esp_matter_endpoint.h ~:728-749 per
the explorer; quote lines in the report.)
- [ ] **Step 2:** cmd_mtep: parse the optional second arg; reject with
+MTERR:1 when `!mt_devtype_variant_ok(id, variant)`; store into
s_staged.variant[i]. The query path prints the 4th field only when
nonzero. Boot rebuild and EPAPPLY call create with the entry's variant.
- [ ] **Step 3:** all three images green; host tests green; commit.

### Task C3: The delegate, AT+MTTEMPLEVELS, docs, 0.3.2

**Files:**
- Modify: `main/main.cpp` (delegate class + per-endpoint label store +
  registration + bridge `mt_matter_temp_levels_set`),
  `main/include/mt_matter.h`, `main/mt_at.c` (handler with quote
  parsing), `docs/AT_MT_SPEC.md` (3.9 variant grammar; new 3.16
  MTTEMPLEVELS; device table variant column note),
  `docs/ARCHITECTURE.md` (fifth trap; v2 blob; delegate), `README.md`
  (table row 19 = cabinet; variant mention), `main/include/mt_at_config.h`
  (MT_FW_VERSION "0.3.2")

**Interfaces:**
- Produces: `extern "C" int mt_matter_temp_levels_set(uint16_t ep,
  const char *const *labels, uint8_t count);` returning 0 or MT_ATTR_ERR_*
  (ENDPOINT/CLUSTER/ATTRIBUTE per the constraint mapping; FAILED on
  internal error), routed through attr_err_to_mterr in the handler.

- [ ] **Step 1:** find CHIP's delegate interface
(connectedhomeip src/app/clusters/temperature-control-server/
supported-temperature-levels-manager.h or the server header: the
explorer saw TemperatureControl::SetInstance and
SupportedTemperatureLevelsIteratorDelegate): implement it in main.cpp
over a static per-endpoint store (up to MT_COMP_MAX_ENDPOINTS entries
of up to 16 labels x 17 bytes), register once before
esp_matter::start(). Quote the interface's virtuals in the report.
- [ ] **Step 2:** the bridge validates endpoint (ERR_ENDPOINT), the
TemperatureControl cluster (ERR_CLUSTER), the SupportedTemperatureLevels
attribute presence (ERR_ATTRIBUTE), copies labels into the store under
ChipStackLock, and marks the attribute dirty so subscriptions refresh
(find the reporting call: MatterReportingAttributeChangeCallback or the
esp_matter equivalent; verify and quote).
- [ ] **Step 3:** the handler: set-only; parse `<ep>` then a
quote-aware label list from the RAW remainder of the args string (do
not at_split_args the label section: commas inside quotes are legal;
write a small local parser: opening quote, bytes until closing quote,
comma between entries; reject empty/oversize/quote-in-label/printable
violations with MT_ERR_BAD_PARAM). Bridge result through
attr_err_to_mterr.
- [ ] **Step 4:** docs per the spec sections 3-4; version 0.3.2;
build_b4 rebuild + strings check; all three images green at task end;
commit.

### Task C4: Library variant plumbing and the cabinet class

**Files (library repo, branch cabinet):**
- Modify: `src/MatterEndPoint.{h,cpp}` (hearthDeclare variant overload,
  registry storage, reconcile comparison), `src/Hearth.cpp` (reconcile
  emits `AT+MTEP=<id>,<variant>` when nonzero; parses the optional 4th
  +MTEP? field)
- Create: `src/MatterEndpoints/MatterTemperatureControlledCabinet.{h,cpp}`
- Test: `test/host/test_cabinet.cpp` + reconcile-test additions
- Modify: `test/host/Makefile`, `.gitignore`

**Interfaces:**
- Produces: `static bool hearthDeclare(MatterEndPoint *ep,
  uint32_t deviceTypeId, uint8_t variant);` (the existing two-arg form
  forwards with variant 0; every existing class unchanged);
  `uint8_t hearthDeclaredVariantAt(size_t i);` for the reconcile;
  the cabinet class per the spec section 5 (upstream dual-begin API +
  `bool setSupportedTemperatureLevelLabels(const char *const *labels,
  uint16_t count)` as the documented Hearth extension sending
  `AT+MTTEMPLEVELS=<ep>,"..."...`).

- [ ] **Step 1: reconcile tests first** (test_reconcile.cpp additions):
a declared variant emits `AT+MTEP=0x0071,1` during rebuild; adopt
matches only when the reported variant equals the declared one
(inject `+MTEP:0,1,0x0071,1` and the 3-field zero-variant form);
mismatch triggers rebuild. Red, then implement the plumbing, green.
- [ ] **Step 2: cabinet class tests** (test_cabinet.cpp, recipe
minimums both modes): TN begin declares variant 0 and drives the four
i16 attributes with upstream's exact double conversion; TL begin
declares variant 1, sends generated "Level <n>" labels via
MTTEMPLEVELS at reconcile then writes SelectedTemperatureLevel;
setSupportedTemperatureLevelLabels sends the exact quoted wire string
(pin one with a comma inside a label); controller URC for
SelectedTemperatureLevel dispatches; failed-write cache discipline;
re-begin refused; the 16-entry/16-char caps enforced host-side with
false returns. Red, implement (mirror upstream's API surface from
~/.arduino15/.../MatterTemperatureControlledCabinet.h; internals are
Hearth-pattern, not upstream's), green.
- [ ] **Step 3:** full suite green; commit(s).

### Task C5: Library integration and 0.3.2

**Files:** `src/Hearth.h`, `test/host/test_matter_umbrella.cpp` +
Makefile deps, `keywords.txt`, `README.md` (20 of 20; parked list now
only the two sub-deferrals + command-forwarding; the labels extension
documented), `library.properties` (0.3.2), `examples/
MatterTemperatureControlledCabinet/` (byte-identical upstream .ino,
cmp + compile for the Challenger FQBN, daemon cleanup after).

- [ ] Umbrella + umbrella-test declarations + keywords + README +
version; example copy/cmp/compile; full suite green; commit.

### Task C6: Bench verification (operator present, no unplugs)

- [ ] Flash rebuilt build_b4 (0.3.2), factory-fresh; compose cabinet
variant 1 + cabinet variant 0 (two endpoints, both modes: fifth trap
live both ways); AT+MTEP? shows `0x0071,1` and plain `0x0071`.
- [ ] MTTEMPLEVELS negative pins: bare form ERROR; bad label +MTERR:1;
ep 99 +MTERR:2; a non-cabinet... compose includes 0x0100 for the
+MTERR:3 pin; the number-variant cabinet gives the +MTERR:4 pin.
- [ ] Set labels ("Cold","Warm,ish","Freeze": the comma case live);
commission; chip-tool reads SupportedTemperatureLevels and shows the
labels verbatim; write SelectedTemperatureLevel from chip-tool and
read back; number-mode setpoint round-trip via AT.
- [ ] Library smoke sketch: TL-mode begin + labels extension +
selected-level callback from a controller write; TN-mode setpoint
setter reaching the wire.
- [ ] Restore bench (espnow bridge explicitly, factory-fresh, 0x0100),
report with verbatim evidence.
