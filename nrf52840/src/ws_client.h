#pragma once
// Shim: upstream ws_client.h → LoRa-only node (no WiFi hardware, no WebSocket).
// The LoRa modules push JSON telemetry (lora_sweep/lora_ch/lora_rx) through this
// surface; here it lands on the serial console instead of a backend socket —
// the DATA batches go over the LoRa uplink (lora_uplink in lora_scan.cpp).
// ws_epoch_now() keeps its upstream role as the fleet-wide UTC clock for the
// LINK channel schedule; the source is `set_time` provisioning instead of WS.
#include <Arduino.h>

bool ws_client_connected();                 // true when a host has the USB-CDC open
bool ws_client_send_raw(const char* json);  // -> serial line "[ws>] {...}"

uint32_t ws_epoch_now();                    // UTC epoch (provisioned) or a free-run base
void     ws_epoch_set(uint32_t epoch);      // serial/BLE cmd set_time
bool     ws_epoch_synced();                 // true after set_time
