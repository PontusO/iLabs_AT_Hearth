# Architecture & Design Decisions — iLabs AT Hearth (ESP32-C6)

Status: **living document.** Consolidates the design of the Matter co-processor
firmware and the decisions taken during Phase B. The phased build plan and the
merge strategy live in the
[ESP-NOW + Matter integration plan](https://github.com/PontusO/iLabs_AT_ESP-now/blob/main/docs/matter-integration-plan.md);
this document is the Matter-firmware view and the decision record.

## 1. What this is

`iLabs_AT_Matter` turns an ESP32-C6 into a **Matter co-processor** driven by a
host MCU (RP2350) over a UART AT interface (`AT+MT…`, see `AT_MT_SPEC.md`). It is
the sibling of `iLabs_AT_ESP-now`, which exposes ESP-NOW over `AT+EN…`.

## 2. Two firmwares, one engine

- **Two single-purpose images, shared design.** ESP-NOW and Matter ship as
  separate binaries that share the `components/at_core` component. The host
  reflashes the C6 over UART to switch personality (see `FIRMWARE_UPDATE_SPEC.md`).
  There is no combined ESP-NOW + Matter binary in v1.
- **`at_core`** (lives in the `iLabs_AT_ESP-now` repo, pulled cross-repo via
  `EXTRA_COMPONENT_DIRS`) provides the subsystem-agnostic AT parser engine
  (`at_parser`), the UART transport (`at_uart`), and the single radio owner
  (`link_mgr`). Each personality registers its own command table
  (`at_register_commands`) and error namespace, so a future merged binary is a
  mechanical "register both tables" step.
- **Matter personality files** (this repo, `main/`):
  - `main.cpp` (C++) — the esp_matter runtime: node + on/off-light endpoint +
    `esp_matter::start()`, the commissioning event callback, and the C-linkage
    `mt_matter_*` bridge into CHIP/esp_matter.
  - `mt_at.c` (C) — the `AT+MT` command handlers on `at_core`.
  - `mt_matter.h` / `mt_at.h` — C-linkage bridges so C (handlers) and C++
    (esp_matter) call each other without name-mangling.

## 3. Transport decision: Matter-over-WiFi (not Thread) for v1

- **WiFi, 2.4 GHz.** BLE is used for commissioning only. Chosen because it needs
  **no Thread Border Router** and reuses the WiFi stack the board already has.
- **Thread analysis (measured):** adding Thread (`c6_wifi_thread`) costs
  **~164 KB** of flash (net, after esp-matter's size-opt profile). Two reasons
  it is deferred, neither of which is the flash:
  1. Thread requires a **Border Router** on-site to be reachable/commissioned;
     WiFi does not.
  2. On the current **4 MB** C6 with dual-OTA it is very tight (~4% free); with
     the single-app partition (below) it fits comfortably, and an **8 MB C6**
     module removes all pressure.
  If Thread is ever required, it is a config addition, not a redesign.
- **Radio ownership:** in this *separate* Matter firmware, `esp_matter::start()`
  brings up WiFi/netif itself, so `at_core`'s `link_mgr` `LINK_MODE_MATTER` path
  is a no-op here and is reserved for the future merged binary (one radio owner).

## 4. Partition layout: single-app + host OTA

- The C6 does **not** self-update; the RP2350 host flashes it (see
  `FIRMWARE_UPDATE_SPEC.md`). So the C6 uses a **single-app partition** (one
  ~3.6 MB app slot) rather than dual-OTA, and disables the Matter OTA Requestor.
  This gives ~1.9 MB free for the ~1.84 MB WiFi+Thread image.
- **Current state:** the firmware still carries the esp-matter light's dual-OTA
  table (copied in during B2); switching to single-app + `ENABLE_OTA_REQUESTOR=n`
  is a pending cleanup that matches this decision.

## 5. UART topology

- **AT link:** `at_core` UART1 on the host-bridge pins **GPIO16/17** (where the
  RP2350 bridge is wired). This is the `AT+MT` channel.
- **Console/logs:** moved to **GPIO2** (RX parked on GPIO4) so boot/Matter logs
  never share a pin with the AT stream. Same arrangement as the ESP-NOW firmware.

## 6. Toolchain

- **ESP-IDF v5.4.1** + **esp-matter release/v1.5** (esp-matter's validated IDF
  for C6). This is *separate* from the ESP-NOW firmware's IDF v5.5.4 — esp-matter
  does not build on 5.5.4 (fails at `chip_gn`). The two firmwares therefore build
  against different IDFs; fine, since they are separate images. `at_core` compiles
  cleanly under both.
- Build: source `~/esp/esp-idf-v5.4.1/export.sh` then `~/esp/esp-matter/export.sh`,
  then `idf.py set-target esp32c6 && idf.py build`. Requires `iLabs_AT_ESP-now`
  checked out as a sibling (provides `at_core`).

## 7. Commissioning

- BLE-commissioned onto a WiFi fabric. Development uses esp-matter test creds
  (discriminator `3840`, passcode `20202021`); `AT+MTCODES?` returns the QR /
  manual codes. Controller for bring-up: `chip-tool` (built with the Avahi mDNS
  backend on the dev host). Production DAC/factory-data story: TBD.

## 8. Repository layout

```
CMakeLists.txt                 esp-matter integration + at_core sibling dir
partitions.csv                 (dual-OTA today; -> single-app, see §4)
sdkconfig.defaults[.esp32c6]   Matter config + console/AT UART split
main/
  main.cpp                     esp_matter runtime + mt_matter_* bridge
  mt_at.c                      AT+MT command handlers on at_core
  include/  mt_at.h, mt_matter.h, mt_at_config.h
fw/  flash.py, RP2350USB2Serial.ino.uf2   host-bridge flasher (PC-side)
docs/  AT_MT_SPEC.md, FIRMWARE_UPDATE_SPEC.md, ARCHITECTURE.md (this file)
```

## 9. Decision log (summary)

| Decision | Choice | Why |
|---|---|---|
| Matter transport | WiFi (not Thread) v1 | No border router; reuse WiFi. |
| Combined ESP-NOW+Matter binary | No (two images) | 4 MB flash; single-purpose; mode-switch by reflash. |
| IDF version | v5.4.1 (Matter) / v5.5.4 (ESP-NOW) | esp-matter release/v1.5 needs 5.4.1. |
| OTA | Host serial flash (single-app) | Robust, flash-efficient, host-owned. |
| Matter OTA Requestor | Disabled | Redundant with host OTA; frees flash. |
| Radio owner (Matter) | esp_matter (link_mgr reserved) | Separate image; link_mgr for the merge. |
