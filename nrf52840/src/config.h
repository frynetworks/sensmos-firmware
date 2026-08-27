#pragma once
// SENSMOS nRF52840 config — subset of upstream config.h relevant to the LoRa-only node.
#include "lora_config.h"

// Wersja: baza upstream 0.90, sufiks -nrf1 odróżnia port w panelu/OTA.
#define FW_BASE "0.90"
#define FW_VERSION FW_BASE "-nrf1"

#define MAX_ENTITY_LEN       28
#define ENTITY_PUB_MAX       16
#define ENTITY_MON_MAX       18
#define ENTITY_OWN_MAX       16
#define ENTITY_TMP_MAX        8
#define ENTITY_POOL_MAX      16

#define OWN_TTL_S          1800
#define PUB_TTL_S          86400

// Przycisk serwisowy: XIAO nRF52840 nie ma wolnego przycisku użytkownika (tylko RST) —
// funkcje przycisku przejmują komendy serial/BLE (factory_reset). Pin celowo nieużywany.
#define SERVICE_BUTTON_PIN    -1
#define SERVICE_BTN_BLE_MS    3000
#define SERVICE_BTN_RESET_MS 10000

#define BATCH_MIN_INTERVAL_MS   (1UL * 60 * 1000)   // min odstęp między batchami
#define BATCH_FORCE_INTERVAL_MS (3UL * 60 * 1000)   // wymuszony batch
#define MON_INTERVAL_MS         (3UL * 60 * 1000)   // ramka telemetrii (na serial w tym porcie)
