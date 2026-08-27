# Bug: web flasher offers no plain-ESP32 LoRa manifest — T-Beam v1.1 reports "not compatible"

Prepared for filing against `Galusz/sensmos-firmware` (upstream) — specifically whatever
serves the `sensmos.com/flash/*.json` ESP Web Tools manifests. Canonical source repo for
the flasher page itself was not confirmed from this sandbox; file against the upstream
firmware repo issue tracker and let the maintainer redirect if the flasher lives
elsewhere. All evidence collected 2026-08-26/27 against the live site.

## Environment

- Web flasher: `https://sensmos.com` "Flash LoRa firmware" button, using ESP Web Tools
  (Web Serial, Chromium-based browsers).
- Target hardware: LilyGO T-Beam v1.1 — plain ESP32 (Xtensa LX6), NOT ESP32-S3, with an
  onboard SX1276/SX1262-class LoRa radio.

## Problem

The only LoRa-capable manifest the flasher serves is `manifest-esp32s3-lora.json`, and it
declares `"chipFamily": "ESP32-S3"` only:

```
$ curl -s https://sensmos.com/flash/manifest-esp32s3-lora.json
{
  "name": "Sensmos Node (LoRa)",
  "version": "latest",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "https://raw.githubusercontent.com/Galusz/sensmos-firmware/main/firmware/sensmos-esp32s3-lora.bin", "offset": 0 }
      ]
    }
  ]
}
```

ESP Web Tools reads the connected chip's family over Web Serial and compares it against
`builds[].chipFamily`. A T-Beam v1.1 (or any plain-ESP32 board with an SX127x/SX126x
radio) identifies as `ESP32`, which does not match `ESP32-S3` — the flasher UI reports
"Improv Wi-Fi not supported" / "This board is not supported by this installer" and
refuses to proceed, even though the underlying firmware fully supports plain ESP32 as a
chip family (see `manifest-esp32.json`, `chipFamily: "ESP32"`, served at the same path
prefix).

## Expected behavior

A plain-ESP32 LoRa manifest should exist alongside the S3 one, so ESP Web Tools can match
`chipFamily: "ESP32"` for LX6-based radio boards (T-Beam and similar). Proposed manifest,
matching the existing schema and mirroring `manifest-esp32.json` / `manifest-esp32s3-lora.json`:

```json
{
  "name": "Sensmos Node (LoRa, ESP32)",
  "version": "latest",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "https://raw.githubusercontent.com/frynetworks/sensmos-firmware/main/firmware/sensmos-esp32-lora.bin", "offset": 0 }
      ]
    }
  ]
}
```

(Path shown against the `frynetworks` fork, since that is where this report and the
corresponding firmware build target were prepared — `docs/upstream-reports` in that repo
carries the ready-to-file text; the raw URL should point at whichever fork/branch the
maintainer treats as canonical for flasher-served binaries once merged.)

## Impact + workaround

Every plain-ESP32 board with a LoRa radio (T-Beam v1.1 and any future SX127x/SX126x
board built on the LX6 chip rather than S3) is unflashable via the public web installer
today — the only path is `arduino-cli`/`esptool` from source, which defeats the purpose
of the web flasher for non-technical users. No workaround exists on the flasher side
short of adding the missing manifest; on the firmware side this report ships alongside a
new `esp32-lora` build target (FQBN identical to the flotilla `esp32` target plus
`-DLORA_ENABLED=1`) and corrected chip-identity strings (`FWD_CHIP`/`OTA_CHIP` now report
`"esp32-lora"` for this build, distinct from `"esp32s3-lora"`), so a compatible binary is
available as soon as the manifest is added.
