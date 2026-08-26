// serial_cmd.cpp — nRF52840 port. Same JSON command protocol as upstream over USB-CDC.
// WiFi commands answer "no_wifi_hw" (the chip has no WiFi); new here: set_time (epoch
// provisioning — replaces NTP for the LINK schedule) and lora_cfg (channel plan that
// upstream receives from the backend over WS).
#include "serial_cmd.h"
#include "data_sender.h"
#include "identity.h"
#include "ble_config.h"
#include "lora_scan.h"
#include "mesh_tx.h"
#include "ws_client.h"
#include "config.h"
#include <ArduinoJson.h>
#include <Preferences.h>

static String serial_buffer = "";

static void serial_respond(const char* status, const char* cmd,
                           const char* msg = nullptr) {
    char resp[256];
    if (msg) {
        snprintf(resp, sizeof(resp),
            "{\"status\":\"%s\",\"cmd\":\"%s\",\"msg\":\"%s\"}",
            status, cmd, msg);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"status\":\"%s\",\"cmd\":\"%s\"}", status, cmd);
    }
    Serial.printf("[serial] %s\n", resp);
}

// ── Handlers ──────────────────────────────────────────────────
static void cmd_no_wifi(JsonDocument& doc, const char* cmd) {
    serial_respond("error", cmd, "no_wifi_hw");
}

static void cmd_set_pin(JsonDocument& doc, const char* cmd) {
    const char* pin = doc["pin"];
    if (!pin || strlen(pin) < 4 || strlen(pin) > 15) {
        serial_respond("error", cmd, "pin_invalid_length"); return;
    }
    if (strcmp(pin, "123456") == 0 || strcmp(pin, "000000") == 0) {
        serial_respond("error", cmd, "pin_too_simple"); return;
    }
    { Preferences p; p.begin("sensmos", false);
      p.putString("ble_pin", pin); p.end(); }
    serial_respond("ok", cmd);
}

static void cmd_set_device_id(JsonDocument& doc, const char* cmd) {
    const char* id = doc["id"];
    if (!id || !identity_set_override(id)) { serial_respond("error", cmd, "bad_id"); return; }
    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"cmd\":\"set_device_id\",\"device_id\":\"%s\"}", g_device_id);
    Serial.printf("[serial] %s\n", resp);
}

static void cmd_set_time(JsonDocument& doc, const char* cmd) {
    uint32_t epoch = doc["epoch"] | 0UL;
    if (epoch < 1600000000UL) { serial_respond("error", cmd, "bad_epoch"); return; }
    ws_epoch_set(epoch);
    serial_respond("ok", cmd);
}

static void cmd_set_owner(JsonDocument& doc, const char* cmd) {
    // Owner binding without the full BLE register ceremony (no backend reachable over
    // LoRa for the challenge round-trip). Stores the wallet address for the batch header.
    const char* owner = doc["owner"];
    if (!owner || strlen(owner) != 42 || strncmp(owner, "0x", 2) != 0) {
        serial_respond("error", cmd, "bad_owner"); return;
    }
    strncpy(g_owner_address, owner, sizeof(g_owner_address) - 1);
    Preferences p; p.begin("sensmos", false);
    p.putString("owner_addr", owner); p.end();
    serial_respond("ok", cmd);
}

static void cmd_unregister(JsonDocument& doc, const char* cmd) {
    const char* owner = doc["owner"];
    if (!owner || strcmp(owner, g_owner_address) != 0) {
        serial_respond("error", cmd, "owner_mismatch"); return;
    }
    memset(g_owner_address, 0, sizeof(g_owner_address));
    Preferences prefs;
    prefs.begin("sensmos", false);
    prefs.remove("owner_addr");
    prefs.end();
    serial_respond("ok", cmd);
    Serial.println("[serial] node unregistered");
}

static void cmd_get_info(JsonDocument& doc, const char* cmd) {
    char pubkey_hex[131];
    identity_get_pubkey_hex(pubkey_hex, sizeof(pubkey_hex));
    // resp/lora are static (.bss), NOT stack: this handler runs in the 4 KB FreeRTOS
    // loop-task stack (shared with SoftDevice) and the get_info chain (resp[720] + lora[224]
    // + lora_link_status_json's b[288] + newlib vsnprintf) overran it → hard fault. The loop
    // task is single-threaded, so a function-static buffer is safe (not reentrant).
    static char lora[224]; String ls;
    lora[0] = 0;
    if (lora_available()) {
        lora_link_status_json(ls);
        snprintf(lora, sizeof(lora), ",\"lora_board\":\"%s\",\"lora_link\":%s",
                 lora_board_name(), ls.c_str());
    }
    static char resp[720];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"cmd\":\"get_info\","
        "\"device_id\":\"%s\","
        "\"eth_address\":\"%s\","
        "\"owner_address\":\"%s\","
        "\"pubkey\":\"%.32s...\","
        "\"registered\":%s,"
        "\"epoch_synced\":%s%s,"
        "\"firmware\":\"" FW_VERSION "\"}",
        g_device_id, g_eth_address, g_owner_address, pubkey_hex,
        strlen(g_owner_address) > 0 ? "true" : "false",
        ws_epoch_synced() ? "true" : "false", lora);
    // Emit with direct writes, NOT Serial.printf("%s", resp): re-formatting the ~450-byte
    // response through Print::printf's internal vararg buffer corrupted the tail (dumped RAM
    // past ~264 B). Serial.print(const char*) writes the bytes straight to the CDC, chunked.
    Serial.print("[serial] ");
    Serial.println(resp);
    Serial.flush();
}

static void cmd_get_token(JsonDocument& doc, const char* cmd) {
    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"cmd\":\"get_token\",\"token\":\"%s\"}",
        g_api_token);
    Serial.printf("[serial] %s\n", resp);
}

static void cmd_send_now(JsonDocument& doc, const char* cmd) {
    data_sender_trigger();
    serial_respond("ok", cmd);
}

static void cmd_done(JsonDocument& doc, const char* cmd) {
    serial_respond("ok", cmd);
    Serial.println("[serial] session ended - restarting in 1s");
    delay(1000);
    NVIC_SystemReset();
}

static void cmd_factory_reset(JsonDocument& doc, const char* cmd) {
    Serial.println("[serial] FACTORY RESET...");
    Preferences prefs;
    prefs.begin("sensmos",      false); prefs.clear(); prefs.end();
    prefs.begin("sensmos_wifi", false); prefs.clear(); prefs.end();
    prefs.begin("sensmos_api",  false); prefs.clear(); prefs.end();
    Serial.println("[serial] storage cleared. restarting in 3s");
    delay(3000);
    NVIC_SystemReset();
}

// lora_cfg — the channel-plan frame upstream gets from the backend over WS, here
// provisioned over serial/BLE. {"cmd":"lora_cfg","on":true,"beacon":true,"slot":0,
//  "min_per_ch":10,"channels":[{"freq":868.1,"bw":125,"sf":7,"cr":5,"sync":52}]}
static void cmd_lora_cfg(JsonDocument& doc, const char* cmd) {
    if (!lora_available()) { serial_respond("error", cmd, "no_radio"); return; }
    LoraLinkCh chans[LORA_LINK_MAX_CH] = {};
    uint8_t n = 0;
    for (JsonObject c : doc["channels"].as<JsonArray>()) {
        if (n >= LORA_LINK_MAX_CH) break;
        chans[n].freq = c["freq"] | 868.1f;
        chans[n].bw   = c["bw"]   | 125.0f;
        chans[n].sf   = c["sf"]   | 7;
        chans[n].cr   = c["cr"]   | 5;
        chans[n].sync = c["sync"] | 0x34;
        chans[n].mode = c["mode"] | 0;
        n++;
    }
    lora_link_set(doc["on"] | true, doc["beacon"] | true, doc["slot"] | 0,
                  doc["beacon_s"] | 0, doc["min_per_ch"] | LORA_LINK_MIN_PER_CH,
                  n ? chans : nullptr, n, nullptr);
    serial_respond("ok", cmd);
}

// set_mesh_cfg — Meshtastic dual-protocol settings.
// {"cmd":"set_mesh_cfg","enabled":true,"channel":"LongFast","psk":"AQ==","portnum":1}
// psk: base64 16/32-byte key, or the one-byte index form ("AQ==" = default key).
static void cmd_set_mesh_cfg(JsonDocument& doc, const char* cmd) {
    const bool     en = doc["enabled"]  | true;
    const char*    ch = doc["channel"];
    const char*    pk = doc["psk"];
    const uint32_t pn = doc["portnum"]  | 0UL;
    if (!mesh_tx_configure(en, ch, pk, pn)) { serial_respond("error", cmd, "bad_psk"); return; }
    serial_respond("ok", cmd);
}

static void cmd_get_mesh_cfg(JsonDocument& doc, const char* cmd) {
    String st;
    mesh_tx_status_json(st);
    char resp[320];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"cmd\":\"get_mesh_cfg\",\"mesh\":%s}", st.c_str());
    Serial.printf("[serial] %s\n", resp);
}

static void cmd_help(JsonDocument& doc, const char* cmd) {
    Serial.println("\n[serial] JSON commands (same protocol as BLE):");
    Serial.println("  {\"cmd\":\"get_info\"} | {\"cmd\":\"get_token\"} | {\"cmd\":\"send_now\"}");
    Serial.println("  {\"cmd\":\"set_time\",\"epoch\":1756000000}");
    Serial.println("  {\"cmd\":\"set_owner\",\"owner\":\"0x...\"} | {\"cmd\":\"unregister\",\"owner\":\"0x...\"}");
    Serial.println("  {\"cmd\":\"set_device_id\",\"id\":\"<64 hex>\"} | {\"cmd\":\"set_pin\",\"pin\":\"...\"}");
    Serial.println("  {\"cmd\":\"lora_cfg\",\"channels\":[{\"freq\":868.1,\"sf\":7}]}");
    Serial.println("  {\"cmd\":\"lora\",\"do\":\"status|sweep|camp|listen|cad|hunt|bg\"}");
    Serial.println("  {\"cmd\":\"get_mesh_cfg\"} | {\"cmd\":\"set_mesh_cfg\",\"enabled\":true,\"channel\":\"LongFast\",\"psk\":\"AQ==\"}");
    Serial.println("  {\"cmd\":\"factory_reset\"} | {\"cmd\":\"done\"} | {\"cmd\":\"help\"}\n");
}

#if LORA_ENABLED
static void cmd_lora(JsonDocument& doc, const char* cmd) {
    const char* what = doc["do"] | "status";
    bool ok = false;

    if      (!strcmp(what, "status")) { lora_status(); ok = true; }
    else if (!strcmp(what, "bg"))     { lora_bg_set(doc["on"] | true); ok = true; }
    else if (!strcmp(what, "sweep"))
        ok = lora_sweep(doc["from"] | 863.0f, doc["to"] | 870.0f, doc["step"] | 0.2f);
    else if (!strcmp(what, "camp"))
        ok = lora_camp(doc["freq"] | 869.525f, doc["sec"] | 60);
    else if (!strcmp(what, "listen"))
        ok = lora_listen(doc["freq"] | 868.1f, doc["bw"] | 125.0f, doc["sf"] | 7,
                         doc["cr"] | 5, doc["sync"] | 0x34, doc["sec"] | 30);
    else if (!strcmp(what, "cad"))
        ok = lora_cad(doc["freq"] | 869.525f, doc["bw"] | 62.5f, doc["sf"] | 8, doc["sec"] | 30);
    else if (!strcmp(what, "hunt"))
        ok = lora_hunt(doc["freq"] | 869.525f, doc["bw"] | 62.5f, doc["sf"] | 8,
                       doc["cr"] | 8, doc["dwell"] | 900);
    else { serial_respond("error", cmd, "unknown_do"); return; }

    if (ok) serial_respond("ok", cmd);
    else    serial_respond("error", cmd, lora_available() ? "busy" : "no_radio");
}
#endif

// ── Dispatch table ────────────────────────────────────────────
typedef void (*cmd_handler_t)(JsonDocument&, const char*);
struct CmdEntry { const char* cmd; cmd_handler_t fn; };

static const CmdEntry CMD_TABLE[] = {
    { "set_wifi",        cmd_no_wifi },
    { "set_backend",     cmd_no_wifi },
    { "set_pin",         cmd_set_pin },
    { "set_device_id",   cmd_set_device_id },
    { "set_time",        cmd_set_time },
    { "set_owner",       cmd_set_owner },
    { "unregister",      cmd_unregister },
    { "get_info",        cmd_get_info },
    { "get_token",       cmd_get_token },
    { "send_now",        cmd_send_now },
    { "done",            cmd_done },
    { "factory_reset",   cmd_factory_reset },
    { "help",            cmd_help },
    { "lora_cfg",        cmd_lora_cfg },
#if LORA_ENABLED
    { "lora",            cmd_lora },
    { "set_mesh_cfg",    cmd_set_mesh_cfg },
    { "get_mesh_cfg",    cmd_get_mesh_cfg },
#endif
};

static void process_json(String json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[serial] JSON parse error");
        return;
    }
    const char* cmd = doc["cmd"];
    if (!cmd) { Serial.println("[serial] missing 'cmd' field"); return; }

    for (const CmdEntry& e : CMD_TABLE) {
        if (strcmp(cmd, e.cmd) == 0) { e.fn(doc, cmd); return; }
    }
    serial_respond("error", cmd, "unknown_cmd");
}

void serial_cmd_init() {
    Serial.println("[serial] ready. type {\"cmd\":\"help\"} for commands.");
}

void serial_cmd_tick() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serial_buffer.length() > 0) {
                process_json(serial_buffer);
                serial_buffer = "";
            }
        } else {
            serial_buffer += c;
            if (serial_buffer.length() > 512) serial_buffer = "";
        }
    }
}
