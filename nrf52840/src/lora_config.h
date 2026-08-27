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

// ── EU868 duty cycle, PER SUB-BAND ────────────────────────────
// ERC 70-03 gives each sub-band its own allowance; a single node-wide counter is
// therefore wrong in both directions — it throttles traffic that is still legal on
// its own band, and it would hide an over-run if two bands were ever summed.
//   g1  868.0-868.6  MHz -> 1%   =  36 s/h  (SENSMOS beacon + SMOSB uplinks @868.1)
//   g4  869.4-869.65 MHz -> 10%  = 360 s/h  (Meshtastic @869.525)
#define DUTY_G1_LIMIT_MS      36000UL
#define DUTY_G4_LIMIT_MS      360000UL

// ══ Worldwide region plans ════════════════════════════════════
// Same table as the ESP32 port (src/lora_config.h) — one row per region, gathering the
// frequencies that used to be EU868 literals scattered through this file. The EU868 row
// carries EXACTLY the previous literal values, so the default build's behaviour and its
// log output do not move by a byte; tools/gate_duty_bands.py only ever runs EU868 and
// must keep passing unmodified.
//
// Meshtastic channel 0 is derived with the upstream formula, meshtastic/firmware
// src/mesh/RadioInterface.cpp:
//   numChannels = floor((freqEnd - freqStart) / (bw/1000))
//   channel_num = hash("LongFast") % numChannels        // hash = djb2, RadioInterface.cpp
//   freq        = freqStart + bw/2000 + channel_num * (bw/1000)
// for the LONG_FAST preset (BW 250 kHz). djb2("LongFast") = 130429955, hence:
//   EU868  869.4 -869.65 -> 1 ch,   ch  0 -> 869.400 + 0.125 + 0.00 = 869.525
//   US915  902.0 -928.0  -> 104 ch, ch 19 -> 902.000 + 0.125 + 4.75 = 906.875
//   AU915  915.0 -928.0  -> 52 ch,  ch 19 -> 915.000 + 0.125 + 4.75 = 919.875
//   AS923  920.0 -925.0  -> 20 ch,  ch 15 -> 920.000 + 0.125 + 3.75 = 923.875
//   IN865  865.0 -867.0  -> 8 ch,   ch  3 -> 865.000 + 0.125 + 0.75 = 865.875
//   KR920  920.0 -923.0  -> 12 ch,  ch 11 -> 920.000 + 0.125 + 2.75 = 922.875
//   RU864  868.7 -869.2  -> 2 ch,   ch  1 -> 868.700 + 0.125 + 0.25 = 869.075
// Band edges and Meshtastic power limits come from the `regions[]` table in that same
// upstream file (regions EU_868, US, ANZ, TH, IN, KR, RU).
//
// The SENSMOS channel is the primary uplink channel of the matching LoRaWAN plan
// (RP002-1.0.4): EU868 868.1 | US915 902.3 | AU915 915.2 | AS923-1 923.2 |
// IN865 865.0625 | KR920 922.1 | RU864 868.9. Power is MIN(regulatory limit, 20 dBm) —
// 20 dBm is the SX1276 PA_BOOST ceiling, so one row serves both radio families.
//
// Band model: two disjoint sub-bands per region, [0] for SENSMOS and [1] for Meshtastic,
// resolved BY FREQUENCY (duty_for_freq) exactly as before.
//   · limit_ms — airtime budget per hour window. EU868/RU864 have a real duty cycle
//     (1% = 36 s/h, 10% = 360 s/h); FCC/AS regions run at 100% (3 600 000 ms), matching
//     `dutyCycle 100` upstream — there the law limits dwell, not duty.
//   · dwell_ms — cap on a SINGLE transmission (FCC 15.247 / AS923: 400 ms). SIMPLIFICATION:
//     enforced on the SENSMOS band, where our SF7/BW125 frames (<=133 B) fit with room to
//     spare. It is deliberately 0 on the Meshtastic band: the LongFast preset is SF11/BW250,
//     about a second on air, so any dwell value there would block all mesh traffic. Upstream
//     Meshtastic transmits that preset unchanged and interoperability requires the same;
//     re-tuning SF/BW "to fit the dwell" would produce frames no neighbour can hear.
struct LoraBand {
    const char* name;
    float       lo, hi;        // MHz, inclusive sub-band edges
    uint32_t    limit_ms;      // airtime budget per hour window
    uint32_t    dwell_ms;      // 0 = no single-transmission cap
};
struct LoraRegion {
    const char* name;
    float    smos_freq;        // SENSMOS uplink/beacon channel
    int8_t   smos_power;       // dBm
    float    mesh_freq;        // Meshtastic LongFast, channel 0
    int8_t   mesh_power;       // dBm
    // The region's WHOLE RF envelope — NOT the same thing as the duty sub-bands below.
    // band[] models airtime ACCOUNTING and deliberately covers only the two sub-bands we
    // transmit on; duty_for_freq has always had a tolerant fallback ("channel outside the
    // table -> charge the strictest band") because a plan may legitimately carry channels
    // we do not account for separately — 867.1 from LORA_BG_CHANNELS is an ordinary EU868
    // channel. "Is transmitting here allowed" is answered by THIS envelope alone. Using
    // band[] for that job rejected perfectly valid EU868 plans.
    float    rf_lo, rf_hi;
    LoraBand band[2];          // [0] SENSMOS band, [1] Meshtastic band
};

// EU868 MUST stay first: it is the default when NVS holds no region, and its band[0]
// (g1) doubles as the "strictest" band that duty_for_freq charges channels outside the
// table to (867.1 from LORA_BG_CHANNELS, for one) — same semantics as before.
// rf_lo/rf_hi is the region's WHOLE ISM band (not the duty sub-bands):
//   EU868 863-870 (ERC 70-03) | US915 902-928 (FCC 15.247) | AU915 915-928 (AS/NZS 4268)
//   AS923-1 920-925 | IN865 865-867 | KR920 920-923.5 | RU864 864-870
#define LORA_REGIONS { \
  { "EU868",  868.1f,    14, 869.525f, 14, 863.0f, 870.0f, \
                                           { { "g1", 868.0f,  868.6f,  DUTY_G1_LIMIT_MS, 0 }, \
                                             { "g4", 869.4f,  869.65f, DUTY_G4_LIMIT_MS, 0 } } }, \
  { "US915",  902.3f,    20, 906.875f, 20, 902.0f, 928.0f, \
                                           { { "us1", 902.0f, 906.0f, 3600000UL, 400 }, \
                                             { "us2", 906.0f, 928.0f, 3600000UL,   0 } } }, \
  { "AU915",  915.2f,    20, 919.875f, 20, 915.0f, 928.0f, \
                                           { { "au1", 915.0f, 919.0f, 3600000UL, 400 }, \
                                             { "au2", 919.0f, 928.0f, 3600000UL,   0 } } }, \
  { "AS923-1",923.2f,    16, 923.875f, 16, 920.0f, 925.0f, \
                                           { { "as1", 920.0f, 923.5f, 3600000UL, 400 }, \
                                             { "as2", 923.5f, 925.0f, 3600000UL,   0 } } }, \
  { "IN865",  865.0625f, 20, 865.875f, 20, 865.0f, 867.0f, \
                                           { { "in1", 865.0f, 865.5f, 3600000UL,   0 }, \
                                             { "in2", 865.5f, 867.0f, 3600000UL,   0 } } }, \
  { "KR920",  922.1f,    14, 922.875f, 14, 920.0f, 923.5f, \
                                           { { "kr1", 920.0f, 922.5f, 3600000UL,   0 }, \
                                             { "kr2", 922.5f, 923.0f, 3600000UL,   0 } } }, \
  { "RU864",  868.9f,    14, 869.075f, 14, 864.0f, 870.0f, \
                                           { { "ru1", 868.7f, 869.0f, DUTY_G1_LIMIT_MS, 0 }, \
                                             { "ru2", 869.0f, 869.2f, DUTY_G1_LIMIT_MS, 0 } } }, \
}
#define LORA_REGION_DEFAULT   0        // EU868

// Kept as the g1 limit: the SENSMOS link plan lives in g1, and existing references
// to this name mean "the budget for the channel the link is transmitting on".
#define LORA_LINK_DUTY_MS_H   DUTY_G1_LIMIT_MS

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

// ══ Meshtastic dual-protocol TX (port addition) ═══════════════
// The same signed batch also leaves the node as Meshtastic packets, so an existing
// mesh (and its MQTT bridge) can carry SENSMOS data without a SENSMOS gateway.
// Parameters are the EU868 "LongFast" preset as computed by meshtastic/firmware:
//   freq  = freqStart(869.4) + bw/2000 + channel_num*(bw/1000), channel_num 0 → 869.525
//   modem = LONG_FAST: BW 250 kHz, SF11, CR 4/5 (MeshRadio.h modemPresetToParams)
//   sync  = 0x2b (RadioLibInterface.h), preamble 16 symbols (RadioInterface.h)
// One packet at a time, TX'd from the Meshtastic sub-band's own duty-cycle budget.
// MESH_FREQ / MESH_TX_POWER are the EU868 row's values: since the region toggle landed,
// the carrier and the power ceiling are read from the active row at TX time, and these
// literals remain as that row's source and as the default.
#define MESH_FREQ             869.525f
#define MESH_BW               250.0f
#define MESH_SF               11
#define MESH_CR               5
#define MESH_SYNCWORD         0x2B
#define MESH_PREAMBLE         16
#define MESH_TX_POWER         LORA_LINK_TX_POWER
#define MESH_HOP_LIMIT        3

#define MESH_TX_DEFAULT       true          // dual-protocol on out of the box
#define MESH_CHANNEL_DEFAULT  "LongFast"
#define MESH_PSK_DEFAULT      "AQ=="        // PSK index 1 = the default channel key
#define MESH_CHUNK_RAW        160           // batch bytes per Meshtastic packet
#define MESH_UPLINK_BUF       2048          // max serialized signed batch
