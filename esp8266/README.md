# Sensmos — ESP8266 Firmware Port

An **ESP8266MOD port** of the Sensmos DePIN sensor-node firmware.

> Based on **[Galusz/sensmos-firmware](https://github.com/Galusz/sensmos-firmware)** (ESP32, Arduino).
> All protocol design, module architecture and node logic are the upstream project's work.
> This repository only adapts that firmware to the ESP8266 (no Bluetooth, single core, ~80 KB RAM)
> and is offered back to the Sensmos project.

Sensmos is a DePIN sensor network: nodes measure the real world, publish to a shared map, trade
data peer-to-peer, and earn the GALU token. The network powers
[UpFromWhere](https://upfromwhere.com). See [sensmos.com](https://sensmos.com).

## Status

| | |
|---|---|
| Version | `0.1.0-esp8266` |
| Build | `pio run` — clean (Flash 62.8 % of 1 MB, RAM 69.2 % static) |
| Hardware | flashed and boot-verified on real ESP8266 (ESP-12 / FT232R, 4 MB flash) |
| Boot QA | 2 consecutive boots, no exceptions / WDT resets, ~17 KB free heap in portal mode |
| Over-the-air portal test | **not** run by the porting harness — see [Onboarding](#onboarding) |

## Onboarding

ESP8266 has no Bluetooth. Onboarding uses a captive portal instead of the Sensmos mobile app's
BLE flow:

1. Power on the node — it creates a WiFi AP named `SENSMOS-xxxxxx`
2. Connect your phone/laptop to that AP
3. Open a browser — the setup page loads automatically (or navigate to 192.168.4.1)
4. Enter your WiFi credentials, backend URL, and owner wallet address
5. Submit — the node saves config, restarts, and connects to your WiFi

The existing Sensmos mobile app (BLE-based) is not compatible with ESP8266 nodes.
Captive-portal support for the app is tracked separately.

### Registration payload

In the BLE flow the phone keeps its own internet connection, so it can POST the signed
registration to the backend immediately. A phone joined to the node's AP **has no internet**, so
the node instead:

* returns the signed payload (`message`, `sig_esp`, `pubkey_esp`, `proof`) on the portal success
  page, with a copy button, and
* persists it, serving it after the node joins your WiFi at `GET http://<node-ip>/node/reg-payload`.

Submit that payload to the Sensmos backend to complete registration. The signed bytes are
identical to the BLE flow's.

## What changed versus upstream

| Area | Upstream (ESP32) | This port (ESP8266) |
|---|---|---|
| Provisioning | BLE GATT (NimBLE), `ble_config.cpp` | Captive portal — softAP + DNS catch-all + HTTP form (`captive_portal.cpp`); same JSON command set (`auth`, `set_device_id`, `register`, `trust_round`, `trust_sign`, `wallet_*`, `wifi_set`, `factory_reset`) |
| Crypto | mbedTLS ECDSA/ECDH/GCM | [micro-ecc](https://github.com/kmackay/micro-ecc) (secp256k1, RFC6979-style deterministic k) + BearSSL (SHA-256, HMAC, AES-128-GCM). **Wire format unchanged**: DER signatures, same HKDF salt/info, same `[ver|seq|tag|ct]` frame |
| Storage | NVS via `Preferences` | LittleFS-backed `PrefsStore` with the same API and the same namespaces/keys (binary values base64-encoded) |
| Async work | FreeRTOS `net_worker` task + queues | Cooperative `net_worker_tick()` — static ring buffers, one job per `loop()` pass |
| Remote terminal | FreeRTOS task + 4 queues | Single-context tick pump; TCP-window backpressure preserved (reads only what it can forward) |
| ICMP / traceroute | `esp_ping` + lwIP raw pcb via `tcpip_callback` | One shared lwIP raw pcb, called directly (NONOS has no tcpip thread); `icmp_ping()` replaces `esp_ping` |
| OTA | dual slot + `Update.rollBack()` | `Updater` single slot — **no rollback**; the post-update WS confirmation is kept and logs when it fails |
| Watchdog | `esp_task_wdt` (120 s) | ESP8266 HW/SW WDT fed from `loop()` + slow-pass logging |
| WiFi region | `esp_wifi_set_country` | NONOS `wifi_set_country` (channels 1–13) |
| Log/format strings | `.rodata` in flash | `PSTR`/`printf_P` — on ESP8266 `.rodata` lives in DRAM, so literals would otherwise eat the heap |
| Buffer sizing | tuned for ~150 KB heap | entity pools, monitor slots, inbox, job queues and TX scratch retuned for ~25 KB; heap gates rescaled to BearSSL's footprint |

Everything else — entity store, script engine, message router, monitors, checknet, punch/STUN,
subscriptions, HTTP API, NTP, node log — is the upstream logic with mechanical include/API swaps.

## Hardware

ESP8266MOD boards with 4 MB flash: NodeMCU v2 / v3, ESP-12E, ESP-12F, Wemos D1 mini.
Flash layout `eagle.flash.4m1m` (1 MB sketch, 1 MB LittleFS). Service button on GPIO0
(3 s → portal mode, 10 s → factory reset), as upstream.

## Build & flash

```bash
pip install platformio          # or: uv tool install platformio
pio run                         # build
pio run -t upload --upload-port COM12   # flash (use your port)
pio device monitor -b 115200    # serial log
```

## Layout

```
src/
  main.cpp             entry point (ported from SENSMOS_Firmware.ino)
  captive_portal.cpp   provisioning portal (replaces ble_config.cpp)
  identity.cpp         secp256k1 keypair, DER signing (micro-ecc + BearSSL)
  ws_enc.cpp           ECDH + HKDF + AES-128-GCM WS channel
  prefs_store.*        Preferences-compatible LittleFS store
  net_worker.cpp       cooperative network job executor
  traceroute.cpp       lwIP raw ICMP: traceroute + ping
  esp8266_compat.h     ESP32→ESP8266 shims (RNG, MAC, heap, chip info)
  WiFi.h WebServer.h HTTPClient.h Preferences.h ESPmDNS.h Update.h mbedtls/
                       header shims so upstream modules compile unchanged
  …                    upstream modules (entity store, scripts, monitors, HTTP API, …)
```

## Known limitations

* **No OTA rollback** — the ESP8266 has a single sketch slot.
* **TLS is heap-gated.** BearSSL with MFLN needs ~9 KB; jobs defer when the heap is below the
  gate, so HTTPS fetches/monitors run less aggressively than on ESP32.
* **Attestation timing.** `trust_round`/`trust_sign` run over the AP's HTTP transport; the
  attestation's `ble_mac` field carries the station MAC. Treat the timing as a liveness/proximity
  signal (the client must join the node's AP), not as the BLE timing channel.
* Over-the-air portal interaction was verified structurally (routes, page size, DNS catch-all) and
  by compile + hardware boot; the browser flow itself is a manual test.

## Upstream project

| | |
|---|---|
| 🌐 Website | https://sensmos.com |
| 🔧 Firmware (ESP32) | https://github.com/Galusz/sensmos-firmware |
| 📱 App | https://github.com/Galusz/sensmos-app |
| 🏠 Home Assistant | https://github.com/Galusz/sensmos-homeassistant |
| 📜 Protocol | https://github.com/Galusz/sensmos-protocol |

## License

MIT — see [LICENSE](LICENSE). The upstream firmware carries no license file; this port is
published in good faith as a contribution back to the Sensmos project, and the upstream author
retains all rights to the original work.
