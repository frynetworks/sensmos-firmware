#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H
#include <Arduino.h>

// nRF52840 port: NimBLE (ESP32) → Adafruit Bluefruit. Same service/characteristic
// UUIDs and JSON command protocol as upstream, so the mobile app pairs unchanged.
#define BLE_SERVICE_UUID    "a7f3bc52-4e1d-4e7a-9c2f-8b5d6e3a1f0c"
#define BLE_CHAR_WRITE_UUID "a7f3bc52-4e1d-4e7a-9c2f-8b5d6e3a1f0d"
#define BLE_CHAR_READ_UUID  "a7f3bc52-4e1d-4e7a-9c2f-8b5d6e3a1f0e"

extern bool g_ble_active;
extern char g_owner_address[43];
extern char g_backend_url[128];

void ble_load_config();  // load persisted config (owner/pin/backend)
void ble_start();
void ble_stop();
void ble_tick();         // dispatch queued command in loop() context
#endif
