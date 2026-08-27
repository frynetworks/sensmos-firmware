<img src="logo.png" alt="Sensmos" height="80">

# Sensmos — ESP32 Firmware

Firmware that turns a cheap ESP32 into a **Sensmos** node — it reads sensors, cryptographically signs its data, runs logic at the edge, and joins the network. Built on the Arduino-ESP32 framework.

> Sensmos is a DePIN sensor network: nodes measure the real world, publish to a shared map, trade data peer-to-peer, and earn the GALU token. This firmware is the node itself.
>
> The network powers **[UpFromWhere](https://upfromwhere.com)** — uptime monitoring as seen from real home connections, with the nodes doing the probing.

## Features

- **Entities** — native (`pub.*`) network-rewarded readings and custom (`own.*`) values you define.
- **On-device identity** — each node generates a secp256k1 keypair locally and **signs every data batch**. The private key never ships in the firmware image.
- **Provisioning & trust** — BLE setup (WiFi credentials, wallet pairing) and a timed Bluetooth **attestation** ceremony proving the node is physical (anti-sybil).
- **Edge script engine** — up to 4-step rules (`if → action`): webhook, push, web fetch, ping/monitor, or a message to another node — all running locally, no cloud.
- **Local HTTP API** — read/write entities directly on the LAN (used by the [Home Assistant integration](https://github.com/Galusz/sensmos-homeassistant) and ESPHome setups).

## Flash it

The easiest way is the **web flasher** (Chrome/Edge): **https://sensmos.com/flash/** — plug the board in over USB and click flash.

Or build from source with the Arduino IDE / `arduino-cli` (open `SENSMOS_Firmware.ino`, target an ESP32 board). Release builds for every chip family go through `build-all.ps1` (radio variants: `-Targets esp32-lora,esp32s3-lora`). Note: `arduino-cli` requires the sketch directory to be named `SENSMOS_Firmware` — from a differently-named clone, build through a junction/symlink of that name.

## Layout

```
SENSMOS_Firmware.ino     entry point
src/
  ble_config.*           BLE provisioning + attestation
  wifi_manager.*         WiFi connect / captive setup
  data_sender.*          batch build + signing + upload
  entity_store.*         pub.* / own.* entities
  http_client_util.*     web fetch / webhooks
  lora_scan.*            LoRa radio (SX1262/SX1276), scan/link, duty cycle   [LORA_ENABLED]
  lora_config.h          radio pinouts, region table, LoRa tuning            [LORA_ENABLED]
  mesh_proto.* mesh_tx.* Meshtastic wire encoder + TX queue                  [LORA_ENABLED]
  config.h               build-time settings
build-all.ps1            multi-target release build (arduino-cli)
```

## LoRa radio builds (`esp32s3-lora`, `esp32-lora`)

The stock firmware compiled with `-DLORA_ENABLED=1` (see `build-all.ps1`) adds a LoRa
radio layer on top of the normal WiFi node — same fleet code, one extra flag; with the
flag off not a single LoRa instruction enters the fleet binaries.

- **Targets**: `esp32s3-lora` (ESP32-S3 boards: XIAO ESP32S3+Wio-SX1262, Heltec V3,
  LilyGO T3-S3, T-Beam S3-Core, RAK3312) and `esp32-lora` (plain ESP32 boards with
  SX1276/SX1262 — including the **LilyGO T-Beam v1.1**, whose radio is powered up
  through its AXP192 PMU at boot). Pinouts are probed at boot from one table
  (`src/lora_config.h`) — one bin per chip family serves every board.
- **Relay role**: with WiFi up, the node listens on the SENSMOS channel and forwards
  every received LoRa frame (e.g. a field node's `SMOSB` batch chunks) to the backend
  over its WebSocket as `lora_rx` batches — the backend verifies the *originating*
  node's batch signature, so relaying needs no extra trust.
- **Dual-stack fallback**: if WiFi stays down for 3 minutes, the node turns its own
  data path to the air — every ECDSA-signed batch transmits both as chunked `SMOSB`
  frames on the SENSMOS channel and as **Meshtastic** packets (LongFast, AES-CTR)
  time-division multiplexed on the same radio, exactly like the nRF52840 port below.
  When WiFi returns the node goes back to WebSocket delivery.
- **Region plans**: `{"cmd":"set_region","region":"US915"}` over serial switches the
  whole radio plan — SENSMOS channel, Meshtastic LongFast channel, TX power caps and
  per-sub-band duty/dwell accounting. Supported: `EU868` (default), `US915`, `AU915`,
  `AS923-1`, `IN865`, `KR920`, `RU864`. Persisted; unknown names answer `bad_region`.
- **Flashing**: the web flasher currently only offers the S3 LoRa image
  (`manifest-esp32-lora.json` for plain ESP32 is staged in `firmware/` pending upstream
  adoption — see `docs/upstream-reports/webflasher-esp32-lora-manifest.md`); meanwhile:
  `esptool --port COMx write_flash 0x0 firmware/sensmos-esp32-lora.bin`

## Ports in this fork

This fork adds alternative node implementations alongside the upstream ESP32 firmware:

### `nrf52840/` — Seeed XIAO nRF52840 port (LoRa/SX1262 sole transport)

PlatformIO port for the XIAO nRF52840 + Wio-SX1262 — a node with **no WiFi hardware**: LoRa is the only radio uplink, BLE + USB-serial handle provisioning. Boot-time pinout probing covers the known XIAO/SX1262 wirings; data batches are ECDSA-signed and leave the node as chunked `0xE1`/`SMOSB` LoRa frames under the region's duty-cycle budget.

**Dual-protocol**: the same signed batch is also transmitted as **Meshtastic** packets (LongFast, AES-CTR channel encryption) time-division multiplexed on the single SX1262 — so an existing mesh and its MQTT bridge can relay SENSMOS data without a SENSMOS gateway. Duty cycle is tracked per sub-band (EU868: g1 868.0–868.6 at 1 % for SENSMOS, g4 869.4–869.65 at 10 % for Meshtastic), so neither protocol can spend the other's airtime allowance. The same `set_region` command as the ESP32 LoRa build selects the worldwide frequency plan (EU868 default). See [nrf52840/README.md](nrf52840/README.md).

### ESP8266 port — moved to its own repo

The ESP8266 port now lives at
[`frynetworks/sensmos-firmware-experimental`](https://github.com/frynetworks/sensmos-firmware-experimental)
(byte-for-byte split, build instructions included). ESP8266's realistic free heap of
28–36 KB sits below the 60 KB baseline assumed for supported boards, so it stays an
experimental target — kept out of this fork to keep the tree clean for upstream PRs.

### `docker/` — containerized node

A Node.js implementation of the same wire protocol for running Sensmos nodes without ESP hardware. See [docker/README.md](docker/README.md).

## Part of the Sensmos project

| | |
|---|---|
| 🌐 Website | https://sensmos.com |
| 📱 App | https://github.com/Galusz/sensmos-app |
| 🏠 Home Assistant | https://github.com/Galusz/sensmos-homeassistant |
| 📜 Protocol | https://github.com/Galusz/sensmos-protocol |
| 💬 Discord | https://discord.gg/ukea386Kqx |

GALU runs on Polygon. © 2026 Sensmos.
