# Firmware Update (OTA) Specification — host-driven serial flash

Status: **draft, design agreed** (not yet implemented). Defines how the ESP32-C6
Matter co-processor is updated in the field. This deliberately does **not** use
Matter's OTA cluster or on-device HTTP OTA; it uses **host-driven serial
flashing** by the RP2350, which suits the co-processor architecture and frees
the C6 from a dual-OTA partition.

## 1. Model

The C6 is a co-processor to the RP2350 host, which owns the firmware lifecycle.
A new C6 image is delivered to the **RP2350's filesystem (8 MB flash)**, verified
there, and then written to the C6 by the RP2350 acting as a serial flasher — the
same ESP ROM bootloader protocol `esptool` / our `fw/flash.py` use, but running
on the host MCU instead of a PC.

```
  new C6 image ──► RP2350 8 MB flash (staged + verified)
                        │  (image known-good before the C6 is touched)
                        ▼
  RP2350 drives C6 EN + IO9 ──► C6 ROM download loader
                        │
                        ▼
  RP2350 (esp-serial-flasher) writes bootloader+partition-table+app over UART
                        │
                        ▼
  RP2350 resets C6 ──► C6 boots new firmware ──► resumes as Matter/WiFi co-proc
```

## 2. Why this model

- **No dual-OTA on the C6 → single-app partition.** On-device OTA (Matter OTA
  *or* HTTP OTA) needs two app slots (you can't overwrite running firmware). By
  flashing from the host, the C6 uses a single ~3.6 MB app slot instead of two
  1.875 MB slots. WiFi + (optional) Thread + the AT layer fit with ~1.9 MB free,
  so flash is a non-issue. See `ARCHITECTURE.md` §partition.
- **Robust / hard to brick.** The image is fully downloaded and verified on the
  RP2350 *before* the C6 is disturbed. A dropped download leaves the running C6
  untouched — just retry. The C6's ROM download loader is immutable, so a failed
  write is always recoverable by re-flashing.
- **Host owns the lifecycle.** The RP2350 decides when to update, keeps the
  golden image, and can **roll back** by re-flashing the previous image it still
  holds. This is what a co-processor should do.
- **Reuses existing plumbing.** The RP2350 already controls the C6's EN/IO9 lines
  (that is how the USB2Serial bridge's auto-reset works for `fw/flash.py`); here
  the RP2350 toggles them itself instead of relaying from a PC.

## 3. Components & responsibilities

### 3.1 C6 firmware (this repo)
- **Single-app partition table** (no `otadata`/`ota_1`): `nvs`, `phy_init`,
  `esp_secure_cert`, `nvs_keys`, `factory` (app, ~3.6 MB), `fctry`.
- `CONFIG_ENABLE_OTA_REQUESTOR=n` (no Matter OTA).
- **(Planned)** an `AT+`-namespaced HTTP(S) download command so the RP2350 can
  fetch an image over the C6's WiFi and stream it to the host over the AT link
  (the C6 has the only network path on this board). See `AT_MT_SPEC.md` §8.

### 3.2 RP2350 host (arduino-pico host library — separate work)
- Integrate **`esp-serial-flasher`** (Espressif's portable host-MCU flasher
  library; has an RP2040/RP2350 port; speaks the ESP ROM protocol over UART).
  Espressif themselves use it to reflash the 802.15.4 RCP in the Thread Border
  Router — the same host-flashes-co-processor pattern.
- Obtain and stage the C6 image in the 8 MB flash; verify integrity before use.
- Drive C6 **EN (reset)** and **IO9 (boot strap)** to enter the ROM download
  loader; write `bootloader.bin` + `partition-table.bin` + `<app>.bin` at their
  offsets; verify; reset the C6 into the app.

## 4. Update flow (over-the-air variant)

1. RP2350 fetches the new C6 image (over WiFi **through the currently-running
   C6**, or via USB/SD) into its 8 MB filesystem.
2. RP2350 verifies the image (checksum and/or signature).
3. RP2350 puts the C6 into download mode (IO9 low, pulse EN).
4. RP2350 flashes bootloader + partition table + app over UART, then verifies.
5. RP2350 resets the C6; it boots the new firmware and resumes as the Matter/WiFi
   co-processor. Brief connectivity downtime during the flash (~tens of seconds);
   the download already completed, so no data is lost if the network drops.

## 5. Image integrity & rollback

- The RP2350 verifies the staged image before flashing (SHA-256 and/or a
  signature the host trusts). The ESP second-stage bootloader also validates the
  app image at boot.
- **Rollback:** the RP2350 retains the previously-known-good image; if the new
  firmware misbehaves, the host re-flashes the old one. (No on-device A/B slot is
  needed because the "other slot" is the host's 8 MB flash.)
- **Secure boot / flash encryption** on the C6: out of scope for v1; note as a
  production hardening item (affects how images are signed and whether the host
  must handle encrypted images).

## 6. Alternatives considered (and why not)

| Option | Needs dual-OTA on C6 | Notes |
|---|---|---|
| **Host serial flash (chosen)** | No (single-app) | Robust, flash-efficient, host-controlled; needs RP2350 flasher. |
| Matter OTA Requestor cluster | Yes | Standard, but hub/DCL-dependent and duplicates what the host does. |
| On-device HTTP(S) OTA (`esp_https_ota`) | Yes | Generic web-server pull, flexible; but dual-OTA is tight on the 4 MB C6 with Thread. |

On the 4 MB C6, dual-OTA + WiFi fits; dual-OTA + Thread is very tight (~4% free).
Host serial flash + single-app avoids that entirely. If on-device OTA is ever
required alongside Thread, spec the **8 MB C6** module.

## 7. Open items

- The C6-side HTTP download command (image transport over the AT link).
- The RP2350-side `esp-serial-flasher` integration (host library).
- Manufacturing / image signing story; secure-boot decision.
- Where images are hosted and how the host authenticates the source.
