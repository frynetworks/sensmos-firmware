#pragma once
// Shim: ESP32 <esp_mac.h> → nRF52840 FICR device address. device_id derivation needs
// 6 stable per-chip bytes; FICR DEVICEADDR is factory-programmed and survives reflash.
#include <Arduino.h>
#include <nrf.h>

typedef enum { ESP_MAC_WIFI_STA = 0 } esp_mac_type_t;

static inline int esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
    (void)type;
    uint32_t lo = NRF_FICR->DEVICEADDR[0];
    uint32_t hi = NRF_FICR->DEVICEADDR[1];
    mac[0] = (uint8_t)(hi >> 8);
    mac[1] = (uint8_t)(hi);
    mac[2] = (uint8_t)(lo >> 24);
    mac[3] = (uint8_t)(lo >> 16);
    mac[4] = (uint8_t)(lo >> 8);
    mac[5] = (uint8_t)(lo);
    return 0;
}
