#pragma once
// Shim: ESP32 <esp_random.h> → nRF52840 hardware TRNG. Same call surface as ESP-IDF.
// SoftDevice-aware: once BLE is up the SoftDevice owns NRF_RNG, so entropy is drawn
// via sd_rand_application_vector_get; before that, the RNG peripheral is driven
// directly (bias correction on).
#include <Arduino.h>
#include <nrf.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>

static inline bool sensmos_sd_enabled(void) {
    uint8_t en = 0;
    return sd_softdevice_is_enabled(&en) == NRF_SUCCESS && en;
}

static inline void esp_fill_random(void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    if (sensmos_sd_enabled()) {
        while (len) {
            uint8_t avail = 0;
            sd_rand_application_bytes_available_get(&avail);
            if (!avail) { delay(1); continue; }
            uint8_t n = (len < avail) ? (uint8_t)len : avail;
            if (sd_rand_application_vector_get(p, n) == NRF_SUCCESS) { p += n; len -= n; }
        }
        return;
    }
    NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk;   // bias correction
    NRF_RNG->EVENTS_VALRDY = 0;
    NRF_RNG->TASKS_START = 1;
    while (len) {
        while (!NRF_RNG->EVENTS_VALRDY) { /* ~120µs/byte */ }
        NRF_RNG->EVENTS_VALRDY = 0;
        *p++ = (uint8_t)NRF_RNG->VALUE;
        len--;
    }
    NRF_RNG->TASKS_STOP = 1;
}

static inline uint32_t esp_random(void) {
    uint32_t r;
    esp_fill_random(&r, 4);
    return r;
}
