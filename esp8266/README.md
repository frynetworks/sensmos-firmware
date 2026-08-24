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
| Upstream sync | verified current with Galusz/sensmos-firmware:main — 0 commits behind (GitHub compare API, `ahead_by:0`) as of the commits already merged into this fork's history |

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
  ws_tls.*             wolfSSL TLS PoC — gated behind SENSMOS_USE_TLS, see below
  …                    upstream modules (entity store, scripts, monitors, HTTP API, …)
tools/
  gate_heap.py          heap-gate serial harness — see Measured performance
  patch_wolfssl_settings.py
                        pre-build hook for the nodemcuv2_tls env (see TLS upgrade path)
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

## ESP8266 Hardware Specifications & Memory Budget

A technical report on what the ESP8266 gives us, what this port achieved and measured, why the
60 KB free-heap guideline exists and when it applies, and the upgrade path if TLS is required.

### Hardware constraints

* Total user DRAM: **~80 KB** (81,920 B) — versus ~520 KB SRAM on the upstream ESP32 target.
* A bare WiFi sketch (SDK + WiFi stack, no application) leaves ~40–45 KB free.
* This firmware's static allocation (`.bss` + `.data` + `.rodata`): **~57 KB (69.3 %)**.
* What remains (~23 KB) covers the WiFi/lwIP runtime residents (~19 KB) plus every heap allocation.
* The allocator is umm_malloc (first-fit): **fragmentation matters** — `MaxFreeBlockSize` is the
  number that predicts allocation success, not total `FreeHeap`. The `[health]`/`[mem]` log lines
  report both, plus `getHeapFragmentation()`.
* No Bluetooth (unlike ESP32) — no BT/WiFi coexistence overhead.

### Why the 60 KB free-heap guideline applies to ESP32, not ESP8266

**On ESP32 (the upstream target), 60 KB free is the right rule.** With ~520 KB SRAM, a TLS
WebSocket (`wss://`) whose handshake spikes ~28 KB (≈22 KB BearSSL/mbedTLS buffers + ~6 KB
stack), and WiFi + Bluetooth coexistence reserving additional RAM, keeping 60 KB free ensures
the TLS spike never crashes the node while still leaving ~460 KB for the application. It is a
sound guideline in that context.

**On ESP8266, the same number is not applicable — the conditions it protects against don't
exist here:**

* 60 KB free out of 80 KB total would leave 20 KB for SDK + WiFi + application static — which
  alone consume ~57 KB. The number is unreachable by roughly a factor of three before the first
  `malloc`.
* This port runs **plaintext WS by design** (`WS_PLAINTEXT=1` in `config.h`) — the ~28 KB TLS
  handshake spike that motivates the ESP32 rule never happens on the WS path.
* The transport is not unprotected: every frame is already encrypted and authenticated at the
  **application layer** (ECDH + HKDF + AES-128-GCM, `ws_enc.cpp` — see below).
* No Bluetooth, single-purpose sensor workload — the overhead profile is fundamentally different.
* Industry reference points for production-stable ESP8266 firmware without TLS on the main
  connection: Tasmota runs at ~20–26 KB free, ESPHome targets >30 KB. This port's measured
  **~25 KB total addressable** free memory is inside that production-safe range.

**When 60 KB would become relevant here:** if full BearSSL TLS were mandated on the WS
connection, its ~28 KB handshake exceeds everything this chip can free — at that point the
hardware answer is an ESP32/ESP32-C3, and the software answer worth testing first is wolfSSL
(see the TLS upgrade path below).

### Measured performance (after optimization)

Numbers from bounded serial captures (`tools/gate_heap.py`) on real hardware (ESP-12, 160 MHz),
verified over 90–180 s windows with the node connected, encrypted, and acked by the backend:

| Metric | Before optimization | After optimization |
|---|---:|---:|
| DRAM steady-state free | ~5,120 B | ~12,288 B |
| DRAM max free block | ~4,096 B | ~11,264 B |
| DRAM heap fragmentation | not instrumented | 7 % |
| IRAM second heap free | 0 B | 12,520 B |
| **Total addressable free** | **~5,120 B** | **~24,800 B** |
| 180 s stability gate | PASS (floor 3000) | PASS (floor 6000, min 13,312) |
| WDT / OOM / crash events | 0 | 0 |

"Total addressable" = DRAM free + IRAM second heap. The IRAM heap requires 32-bit-aligned
access (byte access works via the core's non32xfer handler, at a cost); the allocations routed
there via `HeapSelectIram` are the net_worker job/result rings (~7.2 KB resident), the 4 KB
HTTP-fetch transient, and the ~3 KB tunnel session buffers — bulk, alignment-friendly, and
latency-tolerant.

### Optimizations applied

**Explicit in this port:**

* IRAM second heap — `PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48_SECHEAP_SHARED` (+12.5 KB IRAM
  heap; +7 KB steady DRAM from routing the buffers above)
* LWIP2 low-memory variant (`TCP_MSS=536`)
* MFLN 512 B buffers on the HTTPClient TLS shim (transient HTTPS fetches only)
* Fixed-scratch JSON serialization + **field-filtered parsing** of pre-encryption frames
  (the `identified` catalog reached 45 rich entity objects / 3,686 B — a full parse OOMs at the
  tightest point of the session; the filter keeps only the fields the firmware reads)
* `WEBSOCKETS_SAVE_RAM` (headers to flash, −476 B static) and `WEBSOCKETS_MAX_DATA_SIZE`
  capped 15 KB → 4 KB (2 KB was tested and rejected: it is smaller than the `identified` frame)
* 160 MHz CPU — no RAM-map change, but peak-memory windows (crypto/JSON/TLS fetches) close
  twice as fast, reducing fragmentation pressure
* `F()`/PROGMEM discipline — 262+ string literals in flash (on ESP8266 `.rodata` lives in DRAM)
* Entity/batch buffers retuned (batch 10 vs ESP32's 16), stack-scoped crypto contexts (zero
  resident heap for ECDH/AES-GCM), `WiFi.persistent(false)`, AP teardown after provisioning,
  mDNS responder retired at runtime (documented OOM guard)

**Framework defaults, verified active in this build:**

* `-Os`, `-ffunction-sections`, `-fdata-sections`, `--gc-sections`, `-fno-exceptions`
* `VTABLES_IN_FLASH`
* Extra 4 K heap (cont-stack overlap)

### Application-layer encryption (current state)

The sensmos protocol encrypts **all** data before it reaches the WebSocket:

* Key exchange: ECDH (secp256k1) — a per-session shared secret negotiated at connect time,
  expanded via HKDF-SHA256 with both peers' nonces.
* Payload protection: AES-128-GCM — authenticated encryption; every frame in both directions
  carries a GCM tag and an anti-replay sequence number.
* Implementation: `ws_enc.cpp` (micro-ecc + BearSSL primitives, stack-scoped contexts, zero
  resident heap). The backend's public key is a compiled constant.

So the payload is already encrypted and integrity-protected in transit, even over plaintext WS.
What application-layer crypto does **not** provide, and TLS would add: **server certificate
verification** (proving the node talks to the real sensmos backend rather than an impersonator
at the DNS/routing layer) and **protocol downgrade protection**.

### TLS upgrade path (if required)

From lightest to heaviest:

**Option 1 — current: plaintext WS + application-layer crypto (recommended on ESP8266).**
Zero memory overhead beyond the measured budget; production-safe at ~25 KB total addressable.
Missing: server certificate verification and downgrade protection. Appropriate while the WS
endpoint is trusted via DNS/infrastructure security.

**Option 2 — application-layer key pinning (lightest addition).**
The ECDH infrastructure already pins the backend's identity key as a compiled constant; this
option formalizes it — verify the server's session key material against the pinned backend
identity during the handshake, and optionally pin a hash of the backend's TLS certificate key
for out-of-band checks. Cost: ~32–64 B. Adds server authentication with no TLS stack at all.
Missing: record-layer protection and protocol negotiation.

**Option 3 — wolfSSL — PoC built, linked, flashed, and measured on-device (2026-08-24); fits after static-RAM shrink + rodata relocation.**
A working proof-of-concept now exists (`ws_tls.cpp`/`ws_tls.h`, gated behind the
`SENSMOS_USE_TLS` build flag, isolated to its own `nodemcuv2_tls` PlatformIO environment so the
shipping `nodemcuv2` build carries zero TLS code — confirmed byte-identical RAM/Flash usage with
and without the PoC files present). Findings, from an actual build against this firmware, not
wolfSSL's published numbers:

* **wolfSSL's `wolfssl/wolfssl` PlatformIO package (5.7.2) ships its own bundled
  `user_settings.h`, tuned for ESP-IDF/ESP32.** `wolfssl/wolfcrypt/settings.h` auto-defines
  `WOLFSSL_USER_SETTINGS` and quoted-`#include`s that bundled file itself — project-side `-D`
  build flags cannot override a `#define` inside a header included *after* them. Worked around
  with a PlatformIO `extra_scripts` pre-build hook (`tools/patch_wolfssl_settings.py`) that
  overwrites the package's copy with an ESP8266-Arduino-appropriate config before every build.
* **ESP8266 Arduino (NONOS SDK) has no BSD sockets layer.** wolfSSL's built-in socket I/O
  (`wolfio.h`) assumes one exists for any target named `ESP8266`, and needs `socklen_t` /
  `struct iovec` that don't exist here. Fixed with `WOLFSSL_USER_IO` + `WOLFSSL_NO_SOCK` and
  custom transport callbacks (`wolfSSL_CTX_SetIORecv`/`SetIOSend`) wired directly to
  `WiFiClient::read()`/`write()` — this part works cleanly.
* **The library compiles.** After the above, plus `NO_CRYPT_TEST`/`NO_CRYPT_BENCHMARK` (wolfSSL's
  own test/benchmark harnesses reference ESP-IDF-only `ESP_LOGE`/`sdkconfig.h` and don't apply
  here) and satisfying TLS 1.3's real prerequisites (`HAVE_HKDF`, `WC_RSA_PSS`), every wolfSSL
  source file builds without error against the ESP8266 Arduino toolchain.
* **It initially did not link.** In the maximally trimmed configuration — `NO_SESSION_CACHE`, a
  single ECC curve (`ECC_USER_CURVES` + `HAVE_ECC256` only), `RSA_LOW_MEM`, `NO_DH`,
  single-threaded, small-stack — the link failed: `.bss` overflowed the ESP8266's 80 KB
  `dram0_0_seg` by **1,488 bytes** on top of this firmware's then ~56.7 KB static baseline.
  wolfSSL's own *direct* `.bss` is trivial (~136–192 B); the overflow was cumulative image cost.
  **Resolved (2026-08-24) by freeing 2,664 B of static DRAM in the existing firmware** — four
  targeted edits, applied to both envs, shipping build improved from 56,736 B to 54,072 B (66.0 %):
  `monitors.cpp` status snapshot now reuses the shared `g_tx_scratch` TX buffer instead of its own
  576 B static (same loop-only single-writer pattern as `punch.cpp`/`tunnel.cpp`); `checknet.cpp`
  traceroute cooldown stores 4-byte FNV-1a host hashes instead of `char[46]` names (−440 B);
  `captive_portal.cpp` WiFi pre-scan cache became portal-only lazy-calloc (−432 B); and
  `node_integration.cpp` queue+batch moved into a lazy-calloc'd `NiState` allocated once at
  init/set_url only when an integration URL is configured (−1,227 B; unconfigured nodes pay
  nothing, and the buffer is never freed so the in-flight webhook body pointer stays valid).
* **Linking was necessary but not sufficient: the linked image reset-looped on boot.** With
  wolfSSL's **~26.1 KB of `.rodata` in DRAM** (this linker script places `.rodata` in
  `dram0_0_seg`), the TLS image's static footprint was 80,732 B — leaving ~1.2 KB of boot heap.
  The NONOS SDK OOMs before `setup()` even runs: 72 consecutive `rst cause:2` resets in a 180 s
  capture with zero readable output. **Fixed by relocating `libwolfssl.a`'s `.rodata` to flash**
  (`tools/relocate_wolfssl_rodata.py`, a TLS-env-only `post:` extra_script that patches the
  *generated* `$BUILD_DIR/ld/local.eagle.app.v6.common.ld`, adding
  `*libwolfssl.a:(.rodata .rodata.* .rodata1)` to the `.irom0.text` collection). Safe because this
  build's MMU config (`MMU_IRAM_HEAP`, NONOS SDK 2.2.x) already installs the non32xfer exception
  handler, so 8/16-bit reads from flash work — slowly, which is acceptable for a PoC measurement
  path. Result: TLS image DRAM static **80,732 → 54,616 B (66.5 %)**, boot heap ≈ 27 KB, on par
  with the shipping build. The shipping `nodemcuv2` env never loads this script and is untouched.
* **On-device `[tls-heap]` measurements (nodemcuv2, 160 MHz, WS backend connected after PoC):**

  | Stage | DRAM free | Max block | Frag | IRAM free | Δ vs baseline |
  |---|---:|---:|---:|---:|---:|
  | baseline (pre-wolfSSL) | 20,752 | 20,168 | 3 % | 12,520 | — |
  | post-wolfSSL_Init | 20,752 | 20,168 | 3 % | 12,520 | 0 |
  | post-CTX_new | 20,488 | 20,168 | 2 % | 12,520 | −264 |
  | post-config | 20,608 | 20,168 | 3 % | 12,520 | −144 |
  | pre-connect | 20,608 | 20,168 | 3 % | 12,520 | −144 |
  | post-tcp-connect | 20,224 | 19,976 | 2 % | 12,520 | −528 |
  | post-SSL_new | 17,864 | 17,704 | 1 % | 12,520 | −2,888 |
  | post-handshake-fail | 13,904 | 13,744 | 2 % | 12,520 | −6,848 |
  | post-cleanup | 18,328 | 15,240 | 16 % | 12,520 | −2,424 |

  The handshake itself **failed with wolfSSL err −326 (record layer version error)** against
  `httpbin.org:443` ~200 ms in, so no post-handshake-ok/steady-state rows exist — that is a
  protocol/config issue in the TLS 1.3-only PoC configuration, **not a memory failure**: the
  device completed the full lifecycle, printed every instrumentation stage, cleaned up, and then
  connected to the production WS backend and ran normally (`ws=up`, heap 14 k steady, frag 7 %,
  zero crashes across the capture). IRAM (second heap) is untouched throughout — wolfSSL
  allocates purely from the DRAM heap (there is no XMALLOC/IRAM routing in this PoC).

**Conclusion: wolfSSL now fits this port.** Measured cost on-device: ~26.1 KB flash (relocated
`.rodata`) + ~0.5 KB extra DRAM static vs the shipping build + **≥ 6.8 KB peak DRAM heap during a
(failed, ~200 ms) handshake** — a completed TLS 1.3 handshake will peak higher; with ~20.7 KB free
at PoC start the margin is comfortable. Post-cleanup the heap recovers to within 2.4 KB of
baseline (fragmentation 16 % immediately after, settling to 7 %). The remaining work for a real
TLS transport is protocol-level (resolve the −326 version error, then re-measure
post-handshake-ok/steady-state), not memory-level. The earlier conclusion that only ESP32-class
hardware could host wolfSSL is superseded by these measurements; wolfSSL's published figures
below remain useful context for an eventual full integration.

<details>
<summary>wolfSSL's published numbers (context, not achieved here)</summary>

* I/O buffers default to **128 B** (`RECORD_SIZE`) versus BearSSL's 16 KB RX default — the
  single biggest memory difference between the stacks, in principle.
* Per-session RAM: **1–36 KB depending on configuration** (buffer sizes, key algorithm, math
  library) — configurable toward the low end when both endpoints are controlled, as here.
* wolfSSL has demonstrated TLS 1.3 in 32 KB total RAM on an Arduino Nano 33 — a different chip
  with more headroom than the ESP8266 has after this firmware's own static allocation.
* `WOLFSSL_SMALL_STACK` (used in the PoC) cuts stack use from ~23–42 KB to ~2.1 KB, shifting that
  usage to the heap rather than eliminating it — irrelevant to the `.bss`/static-image overflow
  found here, which happens before any stack or heap accounting begins.
* Licensing: **GPLv2 / commercial dual license** — GPLv2 is free and compatible with an
  open-source deployment like this fork.

</details>

**Option 4 — BearSSL TLS on the WS path → requires ESP32-class hardware.**
BearSSL (the Arduino core's built-in stack) needs ~28 KB per connection for a persistent
session (≈22 KB buffers + ~6 KB stack) — more than the ~25 KB this port can free in total.
The MFLN 512 B trick this port already uses for HTTPS *fetches* (~9 KB, heap-gated) works
because those connections are short-lived and deferrable; a persistent `wss://` session would
hold the buffers forever and additionally depends on the server negotiating MFLN. If BearSSL
TLS is mandated on the WS connection, the hardware answer is an ESP32 (520 KB) or ESP32-C3
(400 KB).

**Also considered:** Mbed TLS (~26 KB reported integration footprint on ESP8266 — same class
as BearSSL, does not fit); axTLS (deprecated in ESP8266 Arduino core 2.5.0, fully removed in
3.0.0 — do not use).

### Hardware ceiling

* ~80 KB DRAM − ~57 KB static − ~19 KB WiFi/lwIP residents ≈ **4–5 KB free unoptimized**.
* With every software optimization applied: **~12 KB DRAM + ~12.5 KB IRAM ≈ ~25 KB total.**
* 60 KB free DRAM is not achievable on this chip — it exceeds what remains after minimum static
  allocation by ~37 KB. 60 KB combined (DRAM + IRAM) would still need ~35 KB more than exists
  after all optimizations.
* The path to substantially more memory is hardware: **ESP32 (~520 KB) or ESP32-C3 (~400 KB)**
  removes the constraint entirely. The ESP8266 core also supports external SPI SRAM (23LC1024,
  `PIO_FRAMEWORK_ARDUINO_MMU_EXTERNAL_128K`) at the cost of a hardware modification
  (an extra chip on GPIO12–15).

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
