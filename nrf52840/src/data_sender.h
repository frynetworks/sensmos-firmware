#pragma once
#include <Arduino.h>
#include "lora_config.h"

// nRF52840 port: same batch cadence as upstream, transport = LoRa uplink.
// FW version lives in config.h here (no WiFi/LoRa variant split — LoRa is the only build).

// Shared TX scratch: batch JSON built in loop() context only (same rule as upstream).
#define TX_SCRATCH_LEN 3072
extern char g_tx_scratch[TX_SCRATCH_LEN];

void data_sender_init();
void data_sender_tick();
void data_sender_trigger();
