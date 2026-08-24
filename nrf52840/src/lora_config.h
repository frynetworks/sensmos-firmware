#pragma once

// ══════════════════════════════════════════════════════════════
// LoRa (SX1262) — nRF52840 port of upstream lora_config.h.
//
// LORA_ENABLED is forced on for this port (platformio.ini) — LoRa is the node's
// SOLE transport, there is no WiFi on this chip.
//
// The radio still mostly LISTENS (band scan, frame RX). LINK mode adds the periodic
// beacon TX plus — new in this port — the signed data-batch uplink (lora_uplink in
// lora_scan.cpp), both budgeted by the same EU868 duty-cycle counter.
// ══════════════════════════════════════════════════════════════

#ifndef LORA_ENABLED
#define LORA_ENABLED 1
#endif

// ESP cores mark ISRs with ICACHE_RAM_ATTR; ARM has no flash-cached ISR problem.
#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR
#endif

// ── Board pinouts (SX1262 on XIAO nRF52840) ───────────────────
// Upstream pattern kept: pins are NOT chosen at compile time. At boot we probe the
// table until the SX1262 answers (RadioLib returns RADIOLIB_ERR_CHIP_NOT_FOUND when
// the register read comes back empty) — one radio bin covers every known wiring.
//
// nRF twist vs ESP32-S3: SPI pins are FIXED by the XIAO variant (SCK=D8, MISO=D9,
// MOSI=D10 for every known SX1262 wiring), so the table only varies the control pins.
// The three rows come from Meshtastic's seeed_xiao_nrf52840_kit variant.h:
//   BTB kit    — Wio-SX1262 on the board-to-board connector (the Meshtastic kit)
//   standalone — Wio-SX1262 wired to the standalone kit header
//   legacy DIY — third-party SX126x modules (EBYTE E22 etc.), XIAO_BLE_LEGACY_PINOUT
// All Wio-SX1262 modules: TCXO 1.8V, DIO2 drives the TX side of the RF switch
// (dio2_rf=1 → setDio2AsRfSwitch), RXEN is a GPIO.

#include <stdint.h>

struct LoraPinout {
    const char* name;
    int8_t nss, dio1, rst, busy, sck, miso, mosi, rxen;   // rxen < 0 = no RX switch pin
    float  tcxo;                                          // TCXO voltage from DIO3
    uint8_t dio2_rf;                                      // 1 = DIO2 is the TX RF switch
};

//                       name              nss    dio1  rst   busy   sck  miso  mosi  rxen  tcxo  dio2
#define LORA_PINOUTS { \
    { "xiao-nrf-btb",    D3,  D0,  D2,  D1,  D8,  D9,  D10,  D4,  1.8f, 1 },  /* Wio-SX1262 B2B kit     */ \
    { "xiao-nrf-kit",    D4,  D1,  D2,  D3,  D8,  D9,  D10,  D5,  1.8f, 1 },  /* standalone kit header   */ \
    { "xiao-nrf-legacy", D0,  D1,  D3,  D2,  D8,  D9,  D10,  D7,  1.8f, 1 },  /* legacy DIY (E22 etc.)   */ \
}

// Escape hatch: LORA_PIN_FORCE=<index> skips probing and forces one row.
#ifndef LORA_PIN_FORCE
#define LORA_PIN_FORCE -1
#endif

// ── Background channel plan (EU863-870) — unchanged from upstream ─────────────
#define LORA_BG_CHANNELS   { 868.1f, 868.3f, 868.5f, 867.1f, 869.525f }
#define LORA_BG_SFS        { 7, 9, 11 }

#define LORA_BG_FREQ      868.1f
#define LORA_BG_BW        125.0f
#define LORA_BG_SF        7
#define LORA_BG_CR        5
#define LORA_BG_SYNC      0x34
#define LORA_BG_LISTEN_S  20

#define LORA_BG_PERIOD_S  300
#define LORA_BG_DEFAULT   true

#define LORA_BUSY_MARGIN_DB  6
#define LORA_SWEEP_SAMPLES  40

// ══ LINK mode (beacon + continuous RX + batch uplink) ══════════
// LoRa-only port: LINK defaults ON — it IS the transport. Upstream keeps it off
// until the backend enables it over WS, which cannot happen here.
#define LORA_LINK_DEFAULT     true
#define LORA_LINK_MAX_CH      6
#define LORA_ENT_PERIOD_S     300
#define LORA_LINK_MIN_PER_CH  10
#define LORA_LINK_GUARD_S     3
#define LORA_LINK_SLOT0_S     10
#define LORA_LINK_SLOT_GAP_S  7
#define LORA_LINK_TX_POWER    14      // dBm — EU868 ERP limit

#define LORA_LINK_DUTY_MS_H   36000UL // 1% of 3600 s = 36 s airtime/h

#define LORA_BEACON_MAGIC     0xE0
#define LORA_BEACON_PREFIX    "SMOS "
#define LORA_RX_BATCH_MAX     6
#define LORA_RX_CAP_PER_MIN   60
#define LORA_RX_HEX_MAX       128

// ── Batch uplink framing (port addition) ──────────────────────
// Signed data batches leave the node as chunked LoRa frames:
//   [0xE1]["SMOSB "][id8][' '][seq hex4][' '][idx]['/'][cnt][' '][base64 chunk]
// 0xE1 = LoRaWAN "Proprietary" space like the beacon's 0xE0, distinct magic so
// receivers can tell beacon from batch. Payload fits LORA_RX_HEX_MAX (128 B).
#define LORA_UPLINK_MAGIC     0xE1
#define LORA_UPLINK_PREFIX    "SMOSB "
#define LORA_UPLINK_CHUNK     96      // base64 payload bytes per frame
#define LORA_UPLINK_BUF       2048    // max serialized signed batch
