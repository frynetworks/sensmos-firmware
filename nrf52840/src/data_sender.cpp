/**
 * SENSMOS Firmware — Data Sender (nRF52840 port).
 * Builds the same batch JSON as upstream, signs it with the node key (ECDSA DER over
 * SHA256 of the serialized payload — the LoRa path has no GCM session with a backend,
 * so authenticity travels WITH the batch), and hands it to the LoRa uplink.
 * WiFi scan / WS ping / mon_batch-over-WS are gone with the WiFi hardware; mon.*
 * telemetry still prints through the ws_client serial shim for tethered debugging.
 */
#include "data_sender.h"
#include "config.h"
#include "entity_store.h"
#include "identity.h"
#include "ble_config.h"   // g_owner_address
#include "ntp_time.h"
#include "ws_client.h"
#include "lora_scan.h"
#include "mesh_tx.h"
#include "log.h"
#include <ArduinoJson.h>

#define MIN_SEND_INTERVAL   BATCH_MIN_INTERVAL_MS
#define FORCE_SEND_INTERVAL BATCH_FORCE_INTERVAL_MS

char g_tx_scratch[TX_SCRATCH_LEN];

// Version tag readable straight from the .bin (upstream pattern — panel reads the file).
__attribute__((used)) const char FW_VERSION_TAG[] = "SENSMOS_FW_VERSION=" FW_VERSION "=END";

static unsigned long g_last_send    = 0;
static unsigned long g_last_health  = 0;
static bool          g_pending_send = false;

#define BASICS_INTERVAL (30UL * 1000)

static void push_basics() {
    char val[32];
    snprintf(val, sizeof(val), "%lu", millis() / 1000);
    entity_push("mon.uptime_s", val, "s");
}

// entities[]/user_data{} from the buffers — logic identical to upstream.
static void build_entity_payload(JsonDocument& doc, int& pub_count, int& user_count) {
    entity_own_prune(OWN_TTL_S);
    entity_pub_prune(PUB_TTL_S);
    pub_count = user_count = 0;
    int count = entity_count();
    if (count == 0) return;

    JsonArray  pub_arr  = doc["entities"].to<JsonArray>();
    JsonObject user_obj = doc["user_data"].to<JsonObject>();
    char eid[36], ev[64], eu[16];
    unsigned long ets;

    for (int i = 0; i < count; i++) {
        entity_get(i, eid, ev, eu, &ets);

        char base_eid[32] = {0};
        const char* src = eid;
        while (strncmp(src, "pub.", 4) == 0) src += 4;
        strncpy(base_eid, src, sizeof(base_eid)-1);

        const bool is_native_eid = entity_is_native(base_eid);
        const bool has_pub_prefix = (strncmp(eid, "pub.", 4) == 0);
        if (has_pub_prefix && !is_native_eid) continue;

        char pub_eid[36] = {0};
        if (is_native_eid && !has_pub_prefix)
            snprintf(pub_eid, sizeof(pub_eid), "pub.%s", base_eid);
        const char* send_eid = (is_native_eid && !has_pub_prefix) ? pub_eid : eid;

        if (strncmp(eid, "pub.", 4) == 0 || is_native_eid) {
            JsonObject e   = pub_arr.add<JsonObject>();
            e["entity_id"] = send_eid;
            e["value"]     = ev;
            e["unit"]      = eu;
            if (ntp_synced()) {
                uint32_t now_s = millis() / 1000;
                e["last_updated"] = (now_s >= ets)
                    ? ntp_unix_time() - (now_s - ets)
                    : ntp_unix_time();
            } else {
                e["last_updated"] = ets;
            }
            pub_count++;
        } else if (strncmp(eid, "own.", 4) == 0) {
            user_obj[eid + 4] = ev;
            user_count++;
        }
    }
}

static void send_batch() {
    if (lora_uplink_pending()) {
        LOGD("net", "batch skipped — previous uplink still in flight");
        g_last_send = millis();
        return;
    }

    push_basics();

    JsonDocument doc;
    doc["type"]          = "batch";
    doc["device_id"]     = g_device_id;
    doc["owner_address"] = g_owner_address;
    doc["timestamp"]     = ntp_synced() ? ntp_unix_time() : (uint32_t)(millis() / 1000);
    doc["firmware"]      = FW_VERSION;

    int pub_count = 0, user_count = 0;
    build_entity_payload(doc, pub_count, user_count);

    size_t flen = serializeJson(doc, g_tx_scratch, TX_SCRATCH_LEN);
    g_last_send = millis();
    if (flen == 0 || flen >= TX_SCRATCH_LEN - 160) {   // leave room for the sig field
        LOGW("net", "batch payload overflow — skipped");
        return;
    }

    // Sign the serialized payload and splice `,"sig":"<DER hex>"` before the final '}'.
    uint8_t hash[32];
    {
        // sha256 over the exact bytes that precede the sig field
        char saved = g_tx_scratch[flen];
        g_tx_scratch[flen] = 0;
        sha256_string(g_tx_scratch, hash);
        g_tx_scratch[flen] = saved;
    }
    uint8_t der[72]; size_t dl = 0;
    if (!identity_sign(hash, der, &dl)) {
        LOGW("net", "batch sign failed — not sent");
        return;
    }
    char sig_hex[145];
    bytes_to_hex(der, dl, sig_hex);
    flen--;                                             // step back over the closing '}'
    flen += snprintf(g_tx_scratch + flen, TX_SCRATCH_LEN - flen,
                     ",\"sig\":\"%s\"}", sig_hex);

    if (lora_uplink_enqueue(g_tx_scratch, flen)) {
        LOGI("net", "batch queued for LoRa uplink: %uB (pub:%d user:%d, sig %uB DER)",
             (unsigned)flen, pub_count, user_count, (unsigned)dl);
        g_pending_send = false;
    } else {
        LOGW("net", "uplink enqueue failed — retry after cooldown");
    }
    // Dual-protocol: the SAME signed bytes also go out as Meshtastic packets, so a
    // nearby mesh node can relay them. Independent of the SMOS queue — a mesh
    // enqueue failure must never hold up the SENSMOS path (and vice versa).
    if (mesh_tx_enabled() && !mesh_uplink_enqueue(g_tx_scratch, flen))
        LOGW("net", "mesh enqueue failed — previous mesh batch still in flight");
    // Tethered debugging: the same signed batch on the serial console.
    ws_client_send_raw(g_tx_scratch);
}

void data_sender_init() {
    static const char* volatile fwtag = FW_VERSION_TAG;
    (void)fwtag;
    g_last_send    = 0;
    g_pending_send = false;
    push_basics();
}

void data_sender_trigger() { g_pending_send = true; }

void data_sender_tick() {
    static unsigned long s_last_basics = 0;
    unsigned long now = millis();
    if (now - s_last_basics >= BASICS_INTERVAL) { s_last_basics = now; push_basics(); }
    if (now - g_last_health >= 60000UL) { g_last_health = now; log_health(); return; }
    bool cooldown = (now - g_last_send >= MIN_SEND_INTERVAL) || (g_last_send == 0);
    bool force    = (now - g_last_send >= FORCE_SEND_INTERVAL) || (g_last_send == 0);
    if ((g_pending_send && cooldown) || force) send_batch();
}
