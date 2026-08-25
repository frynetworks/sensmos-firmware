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
- **Dual-protocol TX**: every signed batch also goes out as **Meshtastic** packets, so an
  existing mesh — and its MQTT bridge — can carry SENSMOS data without a SENSMOS gateway
  existing. Both protocols share one SX1262 by time division: SMOS transmits first in any
  given second, Meshtastic borrows the radio for a single packet in a second SMOS did not
  use, and the radio is always retuned to the SENSMOS channel before the next RX window.
  See "Meshtastic dual-protocol" below.
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

## Meshtastic dual-protocol

The batch that leaves as `0xE1`/`SMOSB` frames on the SENSMOS channel is also chunked
into Meshtastic packets on the Meshtastic channel:

| | SENSMOS | Meshtastic |
|---|---|---|
| Frequency | 868.100 MHz | 869.525 MHz (EU868 LongFast, channel 0) |
| Modem | SF7 BW125 CR4:5 | SF11 BW250 CR4:5 |
| Sync word | 0x34 | 0x2B |
| Preamble | 8 | 16 |
| Framing | `[0xE1]["SMOSB "]…` | 16 B header + AES-CTR `Data` protobuf |

Wire format follows meshtastic/firmware master: `PacketHeader` (`RadioInterface.h`),
nonce = packetId u64 LE ‖ nodeNum u32 LE with CTR counter size 4 (`CryptoEngine.cpp`),
channel hash = `xorHash(name) ^ xorHash(psk)` and the PSK-index expansion
(`Channels.cpp/.h`). The `Data` message is two fields (portnum varint, payload bytes),
hand-encoded — wire-identical to nanopb's output for the same message, without adding a
protobuf generator to the build. NodeNum comes from FICR `DEVICEADDR`, so it is stable
across reboots and tied to the same silicon as the SENSMOS device id.

Chunks are sent as `PortNum` 1 (`TEXT_MESSAGE_APP`) by default, which makes them visible
in any Meshtastic client — the only practical way to observe reception without a
SENSMOS-aware receiver. `PRIVATE_APP` (256) is one config field away.

Configure over serial or BLE (identical JSON on both transports):

```json
{"cmd":"get_mesh_cfg"}
{"cmd":"set_mesh_cfg","enabled":true,"channel":"LongFast","psk":"AQ==","portnum":1}
```

`psk` accepts the Meshtastic forms: `"AQ=="` (PSK index 1 = the default channel key),
a base64 16-byte key (AES-128-CTR) or 32-byte key (AES-256-CTR). Index 0 ("no
encryption") is refused — a node shipping plaintext sensor batches is worse than a
silent one. An unparseable PSK leaves the previous config untouched and answers
`bad_psk`. Settings persist in the `sensmos_mesh` namespace.

**Duty cycle.** Meshtastic airtime is charged to the *same* EU868 1 %/h counter as the
beacon and the SMOSB frames, and SMOS has priority: when the budget runs out, mesh TX
pauses first (`[mesh] TX paused — shared duty budget spent`). SF11 is expensive — one
~300 B batch costs ~1.0 s of SMOS airtime and ~3.4 s of Meshtastic airtime — so a node
sending a batch every three minutes reaches the 36 s/h cap partway through the hour and
throttles until the next window. Note that 869.4–869.65 MHz is legally a 10 % band
(which is why Meshtastic uses it) while 868.1 MHz is 1 %: budgeting both against 1 % is
deliberately conservative, not a regulatory requirement.

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
