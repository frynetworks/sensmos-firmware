// main.cpp — SENSMOS port for Seeed XIAO nRF52840 (LoRa/SX1262 sole transport).
// Modeled on the esp8266 port's main.cpp: setup init chain + cooperative tick loop.
// Init order: storage → identity/crypto (self-tests) → BLE provisioning → LoRa
// (probe + LINK default) → entity store → data pipeline. Heap checkpoints at each
// stage; steady-state heap lines every cycle for the gate.
#include <Arduino.h>
#include "heap_stats.h"
#include "log.h"
#include "identity.h"
#include "aes_gcm.h"
#include "entity_store.h"
#include "data_sender.h"
#include "serial_cmd.h"
#include "ble_config.h"
#include "lora_scan.h"
#include "config.h"
#include <Preferences.h>

bool node_running = false;

// ── Storage round-trip proof (greppable) ──────────────────────
static void storage_selftest() {
    Preferences p;
    if (!p.begin("sensmos_test", false)) {
        Serial.println("[SELFTEST] storage=FAIL begin");
        return;
    }
    uint32_t prev = p.getUInt("boots", 0);
    p.putUInt("boots", prev + 1);
    uint8_t blob[8] = {0xde,0xad,0xbe,0xef,1,2,3,4}, back[8] = {0};
    p.putBytes("blob", blob, 8);
    size_t n = p.getBytes("blob", back, 8);
    p.end();
    bool ok = (n == 8) && (memcmp(blob, back, 8) == 0);
    Serial.printf("[SELFTEST] storage=%s boots=%lu\n", ok ? "PASS" : "FAIL",
                  (unsigned long)(prev + 1));
}

// ── Loop-stall self-check (same idea as the esp8266 port) ─────
static unsigned long s_loop_slowest = 0;
static void loop_health_tick() {
    static unsigned long s_last = 0;
    unsigned long now = millis();
    if (s_last) {
        unsigned long gap = now - s_last;
        if (gap > s_loop_slowest) s_loop_slowest = gap;
        if (gap > 5000) LOGW("loop", "slow pass: %lums (worst %lums)", gap, s_loop_slowest);
    }
    s_last = now;
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);
    delay(100);

    LOGI("boot", "SENSMOS SmartNode v%s (nrf52840, LoRa-only)", FW_VERSION);
    LOGI("boot", "chip nrf52840 @%luMHz", (unsigned long)(SystemCoreClock / 1000000UL));
    heap_stats_print("boot");

    storage_selftest();

    if (!identity_init()) LOGE("boot", "identity init failed");
    aes128_gcm_selftest();
    heap_stats_print("post-crypto");

    ble_load_config();
    ble_start();                 // first sd_ble_enable — the SoftDevice claims its RAM here
    heap_stats_print("post-ble");

    entity_store_init();
    entity_tmp_clear();

#if LORA_ENABLED
    lora_scan_init();            // probes the pinout table; no radio = node runs without LoRa
#endif
    heap_stats_print("post-lora");

    data_sender_init();
    serial_cmd_init();

    node_running = true;
    LOGI("boot", "init complete");
}

void loop() {
    loop_health_tick();
    log_heap_sample();

    serial_cmd_tick();
    ble_tick();
#if LORA_ENABLED
    lora_pump();
#endif
    data_sender_tick();

    static uint32_t s_last = 0;
    if (millis() - s_last >= 5000) {
        s_last = millis();
        heap_stats_print("");
        HeapStats s = heap_stats();
        // Steady-state marker in the esp8266 gate's shape (values in KB).
        Serial.printf("ready — heap %luk free, blk %luk\n",
                      (unsigned long)(s.free_bytes / 1024),
                      (unsigned long)(s.max_block / 1024));
    }
    delay(50);
}
