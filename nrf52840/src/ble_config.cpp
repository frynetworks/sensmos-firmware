// ble_config.cpp — nRF52840 port: NimBLE → Adafruit Bluefruit.
// Kept from upstream: service/char UUIDs, JSON request → notify response protocol,
// auth {pin} gate with per-session nonce, 1-slot command queue (radio-stack callback
// does NO work — the command runs from ble_tick() in loop context), rate limiter.
// Not ported (needs the mobile app + a reachable backend to exercise): the full
// trust ceremony (trust_round/trust_sign) and wallet backup — logged for follow-up.
#include "ble_config.h"
#include "identity.h"
#include "lora_scan.h"
#include "mesh_tx.h"
#include "ws_client.h"
#include "config.h"
#include "log.h"
#include "esp_random.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <bluefruit.h>

// ── Globals ───────────────────────────────────────────────────
bool g_ble_active        = false;
char g_owner_address[43] = {0};
char g_backend_url[128]  = {0};

// ── Internal ──────────────────────────────────────────────────
static BLEService*        s_svc    = nullptr;
static BLECharacteristic* s_char_r = nullptr;   // notify/read
static BLECharacteristic* s_char_w = nullptr;   // write
static bool               s_connected = false;
static bool               s_auth_ok   = false;
static char               s_pin[32]   = {0};
static char               s_nonce[65] = {0};

// 1-slot command queue (upstream pattern): the SoftDevice event context must not
// run JSON parsing or ECDSA — ble_tick() picks the buffer up from loop().
#define CMD_BUF_LEN 576
static char          s_cmd_buf[CMD_BUF_LEN];
static volatile bool s_cmd_pending = false;

// Rate limiter — 10 requests/min, same as upstream.
static unsigned long s_req_times[10] = {0};
static int           s_req_idx = 0;
static bool rate_ok() {
    unsigned long now = millis();
    int n = 0;
    for (int i = 0; i < 10; i++)
        if (s_req_times[i] && now - s_req_times[i] < 60000) n++;
    s_req_times[s_req_idx++ % 10] = now;
    return n < 10;
}

// ── Helpers ───────────────────────────────────────────────────
static void notify(const char* json) {
    if (!s_char_r || !s_connected) return;
    s_char_r->notify(json, strlen(json));
    LOGD("ble", "-> %s", json);
}
static void ble_ok(const char* cmd) {
    char b[64]; snprintf(b, sizeof(b), "{\"status\":\"ok\",\"cmd\":\"%s\"}", cmd);
    notify(b);
}
static void ble_err(const char* cmd, const char* msg) {
    char b[96]; snprintf(b, sizeof(b),
        "{\"status\":\"error\",\"cmd\":\"%s\",\"msg\":\"%s\"}", cmd, msg);
    notify(b);
}

static void gen_nonce() {
    uint8_t rnd[32];
    esp_fill_random(rnd, 32);
    bytes_to_hex(rnd, 32, s_nonce);
}

// "a7f3bc52-...-1f0c" → 16 bytes, little-endian (SoftDevice order).
static bool uuid128(const char* str, uint8_t out[16]) {
    uint8_t raw[16]; int oi = 0;
    for (int i = 0; str[i] && oi < 16; ) {
        if (str[i] == '-') { i++; continue; }
        unsigned v;
        if (sscanf(str + i, "%2x", &v) != 1) return false;
        raw[oi++] = (uint8_t)v; i += 2;
    }
    if (oi != 16) return false;
    for (int i = 0; i < 16; i++) out[i] = raw[15 - i];
    return true;
}

// ── Load config ───────────────────────────────────────────────
void ble_load_config() {
    Preferences p; p.begin("sensmos", true);
    p.getString("ble_pin",     "").toCharArray(s_pin,           sizeof(s_pin));
    p.getString("owner_addr",  "").toCharArray(g_owner_address, sizeof(g_owner_address));
    p.getString("backend_url", "").toCharArray(g_backend_url,   sizeof(g_backend_url));
    p.end();
    if (!strlen(s_pin)) strcpy(s_pin, "123456");
    if (!strlen(g_backend_url)) strcpy(g_backend_url, "lora://uplink");
}

// ── Callbacks (SoftDevice context — queue only, no work) ──────
static void on_connect(uint16_t) {
    s_connected = true; s_auth_ok = false;
    LOGI("ble", "connected");
}
static void on_disconnect(uint16_t, uint8_t reason) {
    s_connected = false; s_auth_ok = false;
    LOGI("ble", "disconnected (0x%02X)", reason);
    // Bluefruit restartOnDisconnect(true) resumes advertising by itself.
}
static void on_write(uint16_t, BLECharacteristic*, uint8_t* data, uint16_t len) {
    if (!len) return;
    if (s_cmd_pending) { LOGW("ble", "busy — write rejected"); return; }
    size_t n = len < CMD_BUF_LEN - 1 ? len : CMD_BUF_LEN - 1;
    memcpy(s_cmd_buf, data, n);
    s_cmd_buf[n] = 0;
    s_cmd_pending = true;
}

// ── Command handling — loop context only ──────────────────────
static void ble_process_cmd() {
    if (!rate_ok()) { ble_err("?", "rate_limit"); return; }
    LOGD("ble", "<- (%d B)", (int)strlen(s_cmd_buf));

    JsonDocument doc;
    if (deserializeJson(doc, s_cmd_buf)) { ble_err("?", "invalid_json"); return; }
    const char* cmd = doc["cmd"];
    if (!cmd) { ble_err("?", "no_cmd"); return; }

    if (!strcmp(cmd, "factory_reset")) {
        ble_ok(cmd); delay(300);
        Preferences p;
        p.begin("sensmos",      false); p.clear(); p.end();
        p.begin("sensmos_wifi", false); p.clear(); p.end();
        p.begin("sensmos_api",  false); p.clear(); p.end();
        delay(500); NVIC_SystemReset();
    }

    // auth {pin} → {ok, device_id, nonce, first_time}
    if (!strcmp(cmd, "auth")) {
        const char* pin = doc["pin"];
        if (!pin) { ble_err(cmd, "missing_pin"); return; }
        bool first;
        { Preferences p; p.begin("sensmos", true); first = !p.isKey("ble_pin"); p.end(); }
        if (first) {
            Preferences p; p.begin("sensmos", false);
            p.putString("ble_pin", pin); p.end();
            strncpy(s_pin, pin, sizeof(s_pin) - 1);
        } else if (strcmp(pin, s_pin)) {
            ble_err(cmd, "wrong_pin"); return;
        }
        s_auth_ok = true;
        gen_nonce();
        char resp[224];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"auth\",\"device_id\":\"%s\","
            "\"nonce\":\"%s\",\"first_time\":%s}",
            g_device_id, s_nonce, first ? "true" : "false");
        notify(resp);
        return;
    }

    if (!s_auth_ok) { ble_err(cmd, "not_authenticated"); return; }

    // set_device_id {id} — identity restore after reflash (upstream semantics).
    if (!strcmp(cmd, "set_device_id")) {
        const char* id = doc["id"];
        if (!id || !identity_set_override(id)) { ble_err(cmd, "bad_id"); return; }
        char newName[24];
        snprintf(newName, sizeof(newName), "SENSMOS-%.6s", g_device_id);
        Bluefruit.setName(newName);
        char resp[160];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"set_device_id\",\"device_id\":\"%s\",\"ble_name\":\"%s\"}",
            g_device_id, newName);
        notify(resp);
        return;
    }

    // register {owner, sig_wallet} — LoRa-only subset: bind the owner wallet and
    // answer with the node's ECDSA signature over "<nonce>:<owner>" so the app can
    // hand proof-of-possession to the backend out-of-band. The upstream WiFi fields
    // are ignored (no WiFi hardware); the on-line challenge round-trip needs a
    // reachable backend, which LoRa cannot provide synchronously.
    if (!strcmp(cmd, "register")) {
        const char* owner = doc["owner"];
        if (!owner || strlen(owner) != 42 || strncmp(owner, "0x", 2) != 0) {
            ble_err(cmd, "bad_owner"); return;
        }
        char msg[120];
        snprintf(msg, sizeof(msg), "%s:%s", s_nonce, owner);
        uint8_t hash[32]; sha256_string(msg, hash);
        uint8_t der[72]; size_t dl = 0;
        if (!identity_sign(hash, der, &dl)) { ble_err(cmd, "sign_failed"); return; }
        char sig_hex[145]; bytes_to_hex(der, dl, sig_hex);

        strncpy(g_owner_address, owner, sizeof(g_owner_address) - 1);
        { Preferences p; p.begin("sensmos", false);
          p.putString("owner_addr", owner); p.end(); }

        char resp[360];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"register\",\"device_id\":\"%s\","
            "\"sig_node\":\"%s\",\"transport\":\"lora\"}",
            g_device_id, sig_hex);
        notify(resp);
        return;
    }

    if (!strcmp(cmd, "set_time")) {
        uint32_t epoch = doc["epoch"] | 0UL;
        if (epoch < 1600000000UL) { ble_err(cmd, "bad_epoch"); return; }
        ws_epoch_set(epoch);
        ble_ok(cmd);
        return;
    }

    if (!strcmp(cmd, "get_info")) {
        char pubkey_hex[131];
        identity_get_pubkey_hex(pubkey_hex, sizeof(pubkey_hex));
        char resp[420];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"get_info\",\"device_id\":\"%s\","
            "\"eth_address\":\"%s\",\"owner_address\":\"%s\",\"pubkey\":\"%.32s...\","
            "\"registered\":%s,\"firmware\":\"" FW_VERSION "\",\"transport\":\"lora\"}",
            g_device_id, g_eth_address, g_owner_address, pubkey_hex,
            strlen(g_owner_address) > 0 ? "true" : "false");
        notify(resp);
        return;
    }

#if LORA_ENABLED
    // Meshtastic dual-protocol config over BLE — same JSON as the serial command,
    // so the app and a tethered console configure the node identically.
    if (!strcmp(cmd, "set_mesh_cfg")) {
        const bool     en = doc["enabled"] | true;
        const char*    ch = doc["channel"];
        const char*    pk = doc["psk"];
        const uint32_t pn = doc["portnum"] | 0UL;
        if (!mesh_tx_configure(en, ch, pk, pn)) { ble_err(cmd, "bad_psk"); return; }
        ble_ok(cmd);
        return;
    }
    if (!strcmp(cmd, "get_mesh_cfg")) {
        String st;
        mesh_tx_status_json(st);
        char resp[320];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"get_mesh_cfg\",\"mesh\":%s}", st.c_str());
        notify(resp);
        return;
    }
#endif

    ble_err(cmd, "unknown_cmd");
}

// ── Lifecycle ─────────────────────────────────────────────────
static uint8_t s_uuid_svc[16], s_uuid_w[16], s_uuid_r[16];

void ble_start() {
    if (g_ble_active) return;
    if (!uuid128(BLE_SERVICE_UUID, s_uuid_svc) ||
        !uuid128(BLE_CHAR_WRITE_UUID, s_uuid_w) ||
        !uuid128(BLE_CHAR_READ_UUID, s_uuid_r)) {
        LOGE("ble", "uuid parse failed");
        return;
    }

    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);   // MTU 247 — app writes JSON blobs
    if (!Bluefruit.begin()) { LOGE("ble", "Bluefruit.begin failed"); return; }
    char name[24];
    snprintf(name, sizeof(name), "SENSMOS-%.6s", g_device_id);
    Bluefruit.setName(name);
    Bluefruit.Periph.setConnectCallback(on_connect);
    Bluefruit.Periph.setDisconnectCallback(on_disconnect);

    s_svc = new BLEService(BLEUuid(s_uuid_svc));
    s_svc->begin();

    s_char_w = new BLECharacteristic(BLEUuid(s_uuid_w));
    s_char_w->setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    s_char_w->setPermission(SECMODE_OPEN, SECMODE_OPEN);
    s_char_w->setMaxLen(512);
    s_char_w->setWriteCallback(on_write);
    s_char_w->begin();

    s_char_r = new BLECharacteristic(BLEUuid(s_uuid_r));
    s_char_r->setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    s_char_r->setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    s_char_r->setMaxLen(512);
    s_char_r->begin();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(*s_svc);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 244);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);                  // 0 = advertise forever

    g_ble_active = true;
    LOGI("ble", "advertising as %s (provisioning service up)", name);
}

void ble_stop() {
    if (!g_ble_active) return;
    Bluefruit.Advertising.stop();
    g_ble_active = false;
    LOGI("ble", "stopped");
}

void ble_tick() {
    if (!s_cmd_pending) return;
    ble_process_cmd();
    s_cmd_pending = false;
}
