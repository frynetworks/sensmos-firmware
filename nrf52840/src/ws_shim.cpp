#include "ws_client.h"
#include "log.h"

// Free-run fallback base: keeps the LINK schedule arithmetic alive (slots, duty-cycle
// windows) before `set_time` arrives. Absolute alignment with other nodes requires
// provisioning — logged as an autonomous port assumption.
#define EPOCH_FREE_RUN_BASE 1700000000UL

static uint32_t s_epoch_base = 0;      // provisioned epoch at s_epoch_ms
static uint32_t s_epoch_ms   = 0;
static bool     s_synced     = false;

bool ws_client_connected() { return (bool)Serial; }

bool ws_client_send_raw(const char* json) {
    if (!Serial) return false;
    Serial.print("[ws>] ");
    Serial.println(json);
    return true;
}

uint32_t ws_epoch_now() {
    if (s_synced) return s_epoch_base + (millis() - s_epoch_ms) / 1000UL;
    return EPOCH_FREE_RUN_BASE + millis() / 1000UL;
}

void ws_epoch_set(uint32_t epoch) {
    s_epoch_base = epoch;
    s_epoch_ms   = millis();
    s_synced     = true;
    LOGI("time", "epoch set: %lu", (unsigned long)epoch);
}

bool ws_epoch_synced() { return s_synced; }
