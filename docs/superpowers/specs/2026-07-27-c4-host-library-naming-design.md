# C4 host library: naming design

Status: agreed, not implemented.
Supersedes the `iLabs_Matter` name used in
`2026-07-26-at-mt-full-api-design.md` §4 and §12.2.

## 1. The problem

Phase C4 delivers an arduino-pico host library whose stated goal is that an
**unmodified arduino-esp32 Matter sketch runs on a Challenger**. That goal and
the project's naming constraint pull in opposite directions.

An unmodified sketch contains, verbatim:

```cpp
#include <Matter.h>
MatterOnOffLight light;
void setup() {
  Matter.begin();
  if (!Matter.isDeviceCommissioned()) { ... }
}
```

The global is `extern ArduinoMatter Matter;` (arduino-esp32 3.3.8,
`libraries/Matter/src/Matter.h:224`). Every identifier there is a compile-time
contract. There is no version of "the sketch is unmodified" in which any of them
changes.

Against that, `CLAUDE.md` states that the firmware, the repo and the artifact
are never named Matter, because the word mark belongs to the Connectivity
Standards Alliance and using it as a product name requires paid adopter
membership.

The previous spec named the host library `iLabs_Matter`, which is a repo and
artifact name containing Matter. This document resolves that.

## 2. The distinction the design rests on

Two kinds of use, with different exposure and different answers.

| | Examples | Nature of the use | Rule |
|---|---|---|---|
| **Identity** | repo name, `library.properties name=`, Library Manager listing, docs titles, banners, release artifacts | Trademark use as a source identifier: it says who made this and what product it is | **Never Matter. Always Hearth.** |
| **Interop symbols** | header filenames, class names, the global object, enum names and values | The compile-time surface of a third-party API we are being compatible with, plus descriptive reference to the protocol spoken | **Verbatim upstream, and unavoidable.** |

Keeping these separate is what allows full source parity and a clean product
name simultaneously, rather than trading one against the other.

## 3. Decisions

| # | Decision | Rationale |
|---|---|---|
| N1 | Identity is Hearth everywhere | §2. Repo `iLabs_Hearth`, package `iLabs Hearth`, matching the `iLabs_ESP-NOW` sibling. |
| N2 | Matter-named symbols are a **closed set defined by upstream**. We implement them and never extend them | Self-enforcing. Survives upstream adding methods. Keeps every symbol we invent off a Matter-named identifier, which is the whole trademark exposure. |
| N3 | Anything we invent is Hearth-named | The complement of N2. Gives the rule a place to put things instead of only a prohibition. |
| N4 | `src/Hearth.h` is canonical; `src/Matter.h` is a one-line shim | Full `#include <Matter.h>` parity, with the product's own header as the real one. |
| N5 | Link, diagnostics and update surface lives on a **second global, `Hearth`** | N2 forbids putting it on `Matter`. See §6 for the divergence from `iLabs_ESP-NOW` this causes. |
| N6 | `Matter.begin()` with no arguments uses a default link | A truly unmodified sketch must work with zero configuration. `Hearth.begin()` before it overrides. |

## 4. Package identity

```
repo:                 iLabs_Hearth
firmware repo:        iLabs_AT_Hearth        (unchanged)

library.properties:
  name=iLabs Hearth
  architectures=rp2040
  includes=Matter.h
  sentence=...bridged to an ESP32-C6 running iLabs Hearth firmware,
           speaking the Matter protocol.
```

This follows the sibling exactly: firmware `iLabs_AT_ESP-now` / host library
`iLabs_ESP-NOW`, therefore firmware `iLabs_AT_Hearth` / host library
`iLabs_Hearth`.

`includes=Matter.h` is what the Arduino IDE inserts on "Include Library", so the
IDE path also produces the parity line.

Matter appears in `sentence` and `paragraph` only as the protocol spoken, which
is the same descriptive use the firmware documentation already makes.

### 4.1 Where the repo lives

`Dropbox/Arduino/libraries/iLabs_Hearth`, a git repo in place inside the
sketchbook. Initialised, branch `main`.

This was briefly treated as a problem on the grounds that `iLabs_ESP-NOW` is a
repo under `~/Data/src/git/` and absent from the sketchbook, and that running
git inside a Dropbox-synced tree risks `.git` corruption. Measurement said
otherwise: **50 of the 219 entries in that sketchbook are already git repos**,
including `iLabs_ST87M01` and `iLabs_nrf52_adrastea`, none carrying Dropbox
ignore attributes, and the arrangement has held since 2024. `iLabs_ESP-NOW` is
the outlier, not the convention.

A symlink from `src/git` into `libraries/` was considered and rejected. There
are **zero symlinks** among those 219 entries, so it would be unprecedented
here, and Dropbox follows directory symlinks and syncs the target's contents,
so it would not have kept `.git` out of Dropbox in any case.

The one real consequence is that the working tree collects Dropbox artefacts,
so `.gitignore` carries `*conflicted copy*` patterns the sibling does not need.

### 4.2 Header shadowing

The library ships `src/Matter.h` while the esp32 core also ships a `Matter`
library. These are kept apart by `architectures=`: ours declares `rp2040`, the
core's declares `esp32`, and the Arduino resolver prefers the architecture
match.

This is **not novel and is already proven here**. `iLabs_ESP-NOW` declares
`includes=ESP32_NOW.h` and ships that header, which the esp32 core also ships,
and the arrangement works today.

## 5. File layout

```
src/Hearth.h                          canonical: declares both surfaces
src/Matter.h                          #pragma once + #include "Hearth.h"
src/HearthLink.{h,cpp}                ATLink, shared with iLabs_ESP-NOW
src/MatterEndPoint.{h,cpp}
src/MatterEndpoints/Matter*.{h,cpp}   filenames mirror upstream verbatim
```

Endpoint filenames mirror upstream because they are part of the interop surface:
a sketch or a third-party header may include them directly.

## 6. The two surfaces

### 6.1 Closed set: Matter-named

Frozen to arduino-esp32's published surface. Whichever of these we implement, we
implement under upstream's name and add nothing to them.

This is a naming rule, not a delivery commitment: C4 still ships only the four
slice device types per §12.2 of the previous spec. The set below is what the
names are reserved for.

- the `Matter` global and `class ArduinoMatter`, including its unlovely name,
  which is upstream's and therefore not ours to improve
- `MatterEndPoint` and the twenty `Matter*` endpoint classes
- `matterEvent_t` with upstream's enumerator names **and values**
- all method signatures on the above

### 6.2 Open set: Hearth-named

Everything with no arduino-esp32 counterpart, because on an ESP32 there is no
link to configure and no co-processor to lose.

- the `Hearth` global
- `Hearth.begin(Stream&, baud)`: link transport override
- `Hearth.onLinkEvent(cb)` and `hearthEvent_t`: link up and down, C6 reboot
  detected, protocol desync
- `Hearth.lastError()`: exposes `+MTERR` detail, which has no upstream analogue
- composition apply and reboot semantics for `AT+MTEPAPPLY`
- serial firmware update entry points per `FIRMWARE_UPDATE_SPEC.md`

### 6.3 Divergence from iLabs_ESP-NOW

The sibling puts its link configuration on the parity global:
`ESP_NOW.setLink(Serial1, channel)`. This design does not, and the two host
libraries will therefore read differently.

The divergence is deliberate. `ESP_NOW` carries no trademark, so hanging
extensions off it cost nothing. `Matter` is precisely the symbol N2 exists to
keep upstream-clean. The cost is real and is accepted: consistency between our
own two libraries is worth less than keeping the Matter-named surface a set we
can point at and say we did not add to it.

## 7. Consequences for the existing spec

### 7.1 `iLabs_Matter` is retired

`2026-07-26-at-mt-full-api-design.md` §4 and §12.2 refer to `iLabs_Matter`. Both
become `iLabs_Hearth`. The §4 phrase "mirrors class names verbatim" stays
accurate and is now the N2 rule.

### 7.2 The esp-matter revision drift loses its naming dimension

§6.3 of the previous spec records that arduino-esp32 3.3.8 bundles esp_matter
1.4.1 while the firmware pins v1.5.1, that namespaces were renamed between them,
and that three Arduino classes name namespaces present in neither revision. It
lists three possible resolutions, one of which was "declaring our own device
type names and mapping them internally".

N2 settles that sub-question without further design. Host class names are frozen
to arduino-esp32's published surface, because that **is** the parity contract;
they are not ours to rename regardless of what any esp_matter revision calls the
underlying namespace. The mapping from host class to Matter device type ID
therefore becomes a Hearth-side table that we version.

`MatterColorLight` and `MatterEnhancedColorLight` both resolving to `0x010D` is
then a table entry rather than an identity problem.

This does **not** resolve the release blocker itself. Whether the normative
revision is fixed by pinning the Arduino core or by owning the table remains
open, and remains a blocker for delivery. What changed is that it is now a
versioning decision rather than a naming one.

## 8. Open, and not naming

- **Which `Serial` and which pins the default link of N6 uses on the
  Challenger.** Needed before `Matter.begin()` can be zero-configuration.
- **§7.2's revision-drift resolution**: pin the core, or own the table. The
  table approach is now tractable, but it is still a decision.
(Repo location was listed here as open and is now settled. See §4.1.)

## 9. Out of scope

- The implementation itself. This document fixes names and boundaries only.
- Endpoint class coverage. §6.2 of the previous spec still governs which device
  types exist.
- Any change to the `AT+MT` wire protocol. `CLAUDE.md` is explicit that the `MT`
  namespace is deliberately not renamed, and nothing here touches it.
