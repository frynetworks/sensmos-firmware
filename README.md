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

Or build from source with the Arduino IDE / `arduino-cli` (open `SENSMOS_Firmware.ino`, target an ESP32 board).

## Layout

```
SENSMOS_Firmware.ino     entry point
src/
  ble_config.*           BLE provisioning + attestation
  wifi_manager.*         WiFi connect / captive setup
  data_sender.*          batch build + signing + upload
  entity_store.*         pub.* / own.* entities
  http_client_util.*     web fetch / webhooks
  config.h               build-time settings
```

## Ports in this fork

This fork adds three alternative node implementations alongside the upstream ESP32 firmware:

### `esp8266/` — ESP8266 firmware port

PlatformIO port of the node firmware for ESP8266MOD boards (NodeMCU v2/v3, Wemos D1 mini, ESP-12). Key differences from the ESP32 target:

- **No BLE** — provisioning happens through a WiFi **captive portal** instead of the Bluetooth setup flow.
- **BearSSL TLS** (MFLN, 512-byte buffers) instead of mbedTLS; micro-ecc for secp256k1 signing — the wire protocol stays byte-compatible.
- **~80 KB DRAM budget** — cooperative `*_tick()` scheduling from `loop()` (no FreeRTOS), `PSTR()`/PROGMEM string placement, LittleFS storage.
- **Heap-exhaustion fixes included** — the registration-window OOM is resolved: the mDNS responder is retired (`MDNS.close()`) after registration or the 120 s onboarding discovery grace (`MDNS_RETIRE_MS`), and TLS peak usage is bounded via BearSSL `setBufferSizes(512, 512)` with a `HEAP_GATE_TLS` heap floor. WiFi association is forced to 802.11g PHY for robust STA/DHCP on 8266 silicon.

Build & flash:

```bash
cd esp8266
pio run                                   # build (NodeMCU v2 env)
pio run -t upload --upload-port <COMx>    # flash
```

See [esp8266/README.md](esp8266/README.md) for the onboarding flow, serial tools, and known limits.

### `nrf52840/` — Seeed XIAO nRF52840 port (LoRa/SX1262 sole transport)

PlatformIO port for the XIAO nRF52840 + Wio-SX1262 — a node with **no WiFi hardware**: LoRa is the only radio uplink, BLE + USB-serial handle provisioning. Boot-time pinout probing covers the known XIAO/SX1262 wirings; data batches are ECDSA-signed and leave the node as chunked `0xE1`/`SMOSB` LoRa frames under the EU868 duty-cycle budget. See [nrf52840/README.md](nrf52840/README.md).

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
