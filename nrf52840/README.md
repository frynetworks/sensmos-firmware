# Sensmos — nRF52840 port (Seeed XIAO nRF52840 + Wio-SX1262)

Port of the Sensmos node firmware to the Seeed XIAO nRF52840 (ARM Cortex-M4F, 256 KB RAM,
SoftDevice S140). The chip has **no WiFi** — **LoRa (SX1262) is the sole transport**;
provisioning runs over **BLE** (same service UUIDs + JSON protocol as the upstream app
flow) and **USB-serial** (same JSON commands).

## What's different from the ESP32 target

- **Transport**: no WebSocket. The upstream LoRa subsystem (band scan, frame RX, LINK-mode
  beacon) is ported intact, and extended with a **signed batch uplink**: each data batch is
  serialized, SHA256-hashed, ECDSA-signed (secp256k1, DER — wire-compatible with upstream),
  and transmitted as chunked LoRa frames `[0xE1]["SMOSB "][id8][seq][idx/cnt][base64]`,
  debited against the same EU868 1%/h duty-cycle counter as the beacon. There is no
  gateway protocol upstream — receiving these frames is a deployment concern.
- **LINK mode defaults ON** (single-channel EU868 plan) — upstream waits for the backend's
  `lora_cfg` over WS, which cannot arrive without WiFi. `{"cmd":"lora_cfg",...}` over
  serial/BLE overrides the default plan.
- **Pin probing**: upstream's boot-time pinout table is kept; the table rows are the three
  known XIAO nRF52840 + SX1262 wirings (B2B kit, standalone kit, legacy DIY). SPI is fixed
  by the variant (SCK=D8, MISO=D9, MOSI=D10). `LORA_PIN_FORCE=<row>` skips probing.
- **Crypto**: micro-ecc (deterministic ECDSA + minimal DER encoder, same as the esp8266
  port), SHA256/HMAC via rweather/Crypto, AES-128-GCM software (`GCM<AES128>`); RNG from
  the nRF TRNG (SoftDevice-aware). CryptoCell-310 is not exposed by the Arduino BSP for GCM.
- **Storage**: `PrefsStore` (ESP32 `Preferences`-compatible) on Adafruit InternalFS
  (LittleFS on internal flash) — survives DFU reflash.
- **Time**: no NTP. The LINK schedule epoch comes from `{"cmd":"set_time","epoch":...}`
  (serial or BLE); before that the node free-runs from a fixed base — slots work, absolute
  fleet alignment needs provisioning.
- **BLE**: NimBLE → Adafruit Bluefruit. auth/set_pin/set_device_id/register(subset)/
  set_time/get_info/factory_reset are ported; the full trust ceremony (trust_round/
  trust_sign) and wallet backup need the mobile app + a reachable backend and are not
  ported yet.

## Build & flash

```bash
cd nrf52840
pio run                                   # build (installs Seeed platform on first run)
pio run -t upload --upload-port <COMx>    # 1200bps-touch serial DFU
```

Note: the bootloader CDC (2886:0045) is not in the board's hwids, so PlatformIO's
port-wait can miss it. `tools/flash_capture.py --flash` does the touch → DFU → capture
sequence resolving ports by VID:PID.

## Tools

- `tools/flash_capture.py` — touch → DFU flash → bounded serial capture from boot.
- `tools/gate_heap_nrf.py` — heap gate / soak: asserts steady state, `[HEAP] free=` ≥
  `--floor` (default 60000), no crash markers, drift ≤ `--drift-max`.

## Serial commands

`{"cmd":"help"}` lists them. Highlights: `get_info`, `send_now` (queue a signed batch for
LoRa uplink), `set_time`, `set_owner`, `lora_cfg`, `lora` (sweep/camp/listen/cad/hunt),
`factory_reset`. WiFi commands answer `no_wifi_hw`.

## Measured (2026-08-24, on hardware)

- Steady-state heap: **173,532 B free** (min over 185 s soak; drift 0 B), max block 95 KB.
- SoftDevice BLE enable cost: ~7 KB. Static RAM 15.3%, flash 37.5%.
- SX1262 probe: detected on the standalone-kit row (`nss D4, dio1 D1, rst D2, busy D3`).
- Signed 305 B batch → 5 LoRa frames @ 868.1 MHz SF7, ~213 ms air each; beacon ~74 ms.
