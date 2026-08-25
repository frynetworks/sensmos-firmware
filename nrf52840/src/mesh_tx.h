#pragma once
#include "lora_config.h"

#if LORA_ENABLED
#include <Arduino.h>
#include <RadioLib.h>

// ══════════════════════════════════════════════════════════════
// Meshtastic uplink — the second half of this node's dual-protocol TX.
//
// The same signed batch that goes out as SMOSB frames on the SENSMOS channel is
// also chunked into Meshtastic packets and transmitted on the Meshtastic channel,
// so a nearby mesh node (and its MQTT bridge) can carry the data to the internet
// without any SENSMOS-specific gateway existing.
//
// One radio, two protocols: mesh TX is time-division multiplexed by lora_scan's
// link_tick — SMOS keeps priority and mesh only ever borrows the radio for a
// single packet at a time, retuning back before the next RX window. Airtime is
// debited from the SAME EU868 duty-cycle counter as beacons and SMOSB frames.
// ══════════════════════════════════════════════════════════════

// Load persisted config ("sensmos_mesh" namespace) — call before lora_scan_init().
void mesh_tx_init();

bool mesh_tx_enabled();
void mesh_tx_status_json(String& out);

// Apply + persist config. psk_b64/channel may be nullptr to keep the current value.
// Returns false when the PSK cannot be parsed (config left untouched).
bool mesh_tx_configure(bool enabled, const char* channel, const char* psk_b64,
                       uint32_t portnum);

// Queue one signed batch. Chunked into as many Meshtastic packets as needed.
// false = disabled, empty, too large, or a batch already in flight.
bool mesh_uplink_enqueue(const char* json, size_t len);
bool mesh_uplink_pending();

// Transmit ONE pending chunk. Called from the radio task only (lora_scan link_tick).
// The caller owns the radio's SENSMOS configuration and must restore it afterwards;
// this function leaves the radio on the Meshtastic parameters.
//   duty_ms/duty_budget — shared EU868 counter, checked before TX, not modified here.
// Returns airtime in ms (0 = nothing sent: no budget, TX error, or nothing pending).
uint32_t mesh_tx_next(SX1262& radio, uint32_t duty_ms, uint32_t duty_budget,
                      float tcxo, int8_t rxen_pin, bool dio2_rf);
#endif
