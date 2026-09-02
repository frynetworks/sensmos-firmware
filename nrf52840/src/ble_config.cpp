// ble_config.cpp — nRF52840 port: NimBLE → Adafruit Bluefruit.
// Kept from upstream: service/char UUIDs, JSON request → notify response protocol,
// auth {pin} gate with per-session nonce, 1-slot command queue (radio-stack callback
// does NO work — the command runs from ble_tick() in loop context), rate limiter.
// Ported from src/ble_config.cpp: the canonical `register` challenge-response and the
// full trust ceremony (trust_round/trust_sign) + wallet backup. The signed byte strings
// are identical to the ESP32's — the backend verifies those exact bytes.
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

// ── Trust ceremony (W2) ───────────────────────────────────────
// Challenge-response rounds (timing) + node-key signature over the attestation.
// App: trust_round xN → trust_sign {seed, owner} → evidence to the backend.
#define TRUST_MAX_ROUNDS 8
// Prefs namespace for the wallet copy — survives factory reset (cleared by hand only).
// Verified: factory_reset below clears sensmos / sensmos_wifi / sensmos_api, never this.
#define NVS_NS_WALLET "wallet_bak"
// Budget per round = max c (64) + r_hex (64); sized for the longest legal challenge.
#define ROUNDS_BUF_LEN (TRUST_MAX_ROUNDS * 128 + 1)

// Handler scratch. The BLE command runs from ble_tick() on the loop task, whose stack is
// 4 KB and shared with the SoftDevice; the get_info chain already overran it once
// (see serial_cmd.cpp:97). So: no large stack arrays. Upstream heap-allocates this union
// in ble_start() because its WiFi mode never opens BLE and the buffers would be dead .bss.
// That trade does not exist here — this node runs BLE and LoRa concurrently for its whole
// life (main.cpp:71/97) — so a plain static is the better deal: same footprint, no
// allocation-failure path, and it matches s_cmd_buf below. Sizes are copied from upstream
// verbatim so truncation behaves identically on both platforms.
// The commands are strictly request-response, so sharing one union across handlers is safe.
union BleBuf {
    struct { char resp[256]; } auth;
    struct { char message[256]; char sig_esp_hex[145]; char pubkey_hex[131];
             char proof_input[512]; char resp[512]; } reg;
    struct { char input[140]; } round;
    struct { char attest[480]; char sig_hex[145]; char pubkey_hex[131]; char resp[512]; } sign;
    struct { char resp[700]; } wallet;
};
static BleBuf s_buf;
static char   s_rounds_buf[ROUNDS_BUF_LEN];
static int    s_rounds_count = 0;

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

// Advertised BLE address, lowercase colon-separated — this is what the phone saw, and the
// attestation binds to it. sd_ble_gap_addr_get returns the address little-endian (see
// bluefruit.cpp:1062 "MAC is in little endian --> print reverse"), so emit it reversed.
// Not FICR DEVICEADDR: a static random address has the top two bits of the MSB forced set,
// so the raw FICR bytes would not match the address the phone observed.
static void ble_mac_str(char out[18]) {
    uint8_t a[6] = {0};
    Bluefruit.getAddr(a);
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             a[5], a[4], a[3], a[2], a[1], a[0]);
}

// ── Canonical ceremony strings ────────────────────────────────
// These three helpers are the ONLY place the signed/hashed bytes are built, and the boot
// self-test exercises these same functions — so a format drift cannot pass unnoticed.
// They must stay byte-identical to src/ble_config.cpp; the backend verifies the exact
// bytes, and app / firmware / backend each rebuild the string independently.

// src/ble_config.cpp:338-341. `ts` is the literal integer 0, NOT a timestamp.
static void ceremony_register_msg(char* out, size_t n, const char* device_id,
                                  const char* owner, const char* nonce) {
    snprintf(out, n, "{\"device_id\":\"%s\",\"owner\":\"%s\",\"nonce\":\"%s\",\"ts\":0}",
             device_id, owner, nonce);
}
// src/ble_config.cpp:358-364 — bare concatenation, no separators.
static void ceremony_proof_hex(char out[65], char* scratch, size_t scratch_n,
                               const char* nonce, const char* sig_hex, const char* device_id) {
    snprintf(scratch, scratch_n, "%s%s%s", nonce, sig_hex, device_id);
    uint8_t h[32]; sha256_string(scratch, h);
    bytes_to_hex(h, 32, out);
}
// src/ble_config.cpp:403-408 — r = sha256(c || device_id).
static void ceremony_round_hex(char out[65], char* scratch, size_t scratch_n,
                               const char* c, const char* device_id) {
    snprintf(scratch, scratch_n, "%s%s", c, device_id);
    uint8_t h[32]; sha256_string(scratch, h);
    bytes_to_hex(h, 32, out);
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
    ble_ceremony_selftest();   // pure string/hash check — see ble_config.h
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

// Fills the 1-slot queue. The check-then-act on s_cmd_pending is NOT atomic, so every
// caller must run on the same task — see the producer-context note in ble_start(). The
// consumer (ble_tick, loop task) only ever clears s_cmd_pending after the command has been
// fully handled, so a single producer cannot race the consumer either.
static void queue_cmd(const uint8_t* data, uint16_t len) {
    if (!len) return;
    if (s_cmd_pending) { LOGW("ble", "busy — write rejected"); return; }
    // A completed long write can hand up 1024 B (the framework's reassembly buffer);
    // clamp to the slot. The largest real command, register, is ~305 B.
    size_t n = len < CMD_BUF_LEN - 1 ? len : CMD_BUF_LEN - 1;
    memcpy(s_cmd_buf, data, n);
    s_cmd_buf[n] = 0;
    s_cmd_pending = true;
}

// Serves two delivery paths, both on the "BLE" task: a completed long write (the framework
// calls this itself from the EXEC_WRITE_REQ_NOW case) and write-without-response, which the
// SoftDevice reports as an ordinary BLE_GATTS_EVT_WRITE. Authorized Write Requests do NOT
// arrive here — see on_write_authorize.
static void on_write(uint16_t, BLECharacteristic*, uint8_t* data, uint16_t len) {
    queue_cmd(data, len);
}

// Write-authorize callback — this is what switches BLE long write ON.
// The framework already implements Prepared Write reassembly (1024 B lazy buffer, one
// completed-payload callback, BLECharacteristic.cpp:454-513) but it is unreachable unless
// attr_meta.wr_auth is set, and wr_auth is set ONLY as a side effect of installing this
// callback. Without it every command over one MTU (247 B, the framework ceiling) is lost —
// which is why the 305 B register failed.
// TRAP: turning wr_auth on also routes ordinary short Write Requests through authorization,
// and the framework does not auto-reply to those. Anything that is not a PREP/EXEC op must
// be answered here or the write stalls. Modelled on BLEDfu.cpp:88-107.
static void on_write_authorize(uint16_t conn_hdl, BLECharacteristic* chr,
                               ble_gatts_evt_write_t* request) {
    if (request->handle != chr->handles().value_handle) return;
    // PREP / EXEC belong to the framework's reassembly path: it replies and, on EXEC,
    // invokes on_write with the whole payload. Replying here too would abort the transaction.
    if (request->op == BLE_GATTS_OP_PREP_WRITE_REQ ||
        request->op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW ||
        request->op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL) return;

    ble_gatts_rw_authorize_reply_params_t reply = { .type = BLE_GATTS_AUTHORIZE_TYPE_WRITE };
    reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
    reply.params.write.update      = 1;    // mandatory for write authorization (ble_gatts.h:301)
    reply.params.write.offset      = request->offset;
    reply.params.write.len         = request->len;
    reply.params.write.p_data      = request->data;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply);

    // The authorization event REPLACES BLE_GATTS_EVT_WRITE for this operation, so the
    // payload has to be taken here — on_write will not fire for it. Two independent
    // confirmations in the framework: ble_gatts.h:301-303 makes `update` mandatory because
    // "the data to be written needs to be stored and later provided by the application",
    // and the EXEC_WRITE_REQ_NOW case at BLECharacteristic.cpp:497-505 invokes _wr_cb by
    // hand after replying — which would double-fire if the SoftDevice also raised a write
    // event. BLEDfu likewise consumes its payload here and registers no write callback.
    queue_cmd(request->data, request->len);
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
        gen_nonce();               // fresh nonce on every auth
        s_rounds_buf[0] = '\0';    // fresh trust ceremony
        s_rounds_count  = 0;

        char* resp = s_buf.auth.resp;
        snprintf(resp, sizeof(s_buf.auth.resp),
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

    // register {owner, sig_wallet, backend_url, ssid, password} — src/ble_config.cpp:308-386.
    // sig_wallet is carried by the app to the backend and is NOT verified here (upstream
    // does not either). ssid/password are validated for payload parity with the ESP32 but
    // are inert: this node has no WiFi, it uplinks over LoRa.
    if (!strcmp(cmd, "register")) {
        const char* owner       = doc["owner"];
        const char* sig_wallet  = doc["sig_wallet"];
        const char* backend_url = doc["backend_url"];
        const char* ssid        = doc["ssid"];

        if (!owner || strlen(owner) < 20)  { ble_err(cmd, "missing_owner"); return; }
        if (!sig_wallet)                   { ble_err(cmd, "missing_sig_wallet"); return; }
        if (!ssid || !strlen(ssid))        { ble_err(cmd, "missing_ssid"); return; }
        if (!backend_url || strlen(backend_url) < 7) { ble_err(cmd, "missing_backend"); return; }

        strncpy(g_owner_address, owner,       sizeof(g_owner_address) - 1);
        strncpy(g_backend_url,   backend_url, sizeof(g_backend_url) - 1);
        LOGI("ble", "config: backend=%s owner=%.10s... (ssid ignored — LoRa uplink)",
             backend_url, owner);

        Preferences p; p.begin("sensmos", false);
        p.putString("owner_addr",  owner);
        p.putString("backend_url", backend_url);
        p.end();

        // Build the message FIRST — this is what gets signed, and what the backend
        // re-derives to verify(message, sig_esp, pubkey_esp).
        char* message = s_buf.reg.message;
        ceremony_register_msg(message, sizeof(s_buf.reg.message), g_device_id, owner, s_nonce);

        uint8_t msg_hash[32];
        sha256_string(message, msg_hash);

        uint8_t sig_raw[72]; size_t sig_len = 0;
        char* sig_esp_hex = s_buf.reg.sig_esp_hex; sig_esp_hex[0] = 0;
        if (identity_sign(msg_hash, sig_raw, &sig_len)) {
            bytes_to_hex(sig_raw, sig_len, sig_esp_hex);
        }

        char* pubkey_hex = s_buf.reg.pubkey_hex;
        identity_get_pubkey_hex(pubkey_hex, sizeof(s_buf.reg.pubkey_hex));

        char proof_hex[65] = {0};
        ceremony_proof_hex(proof_hex, s_buf.reg.proof_input, sizeof(s_buf.reg.proof_input),
                           s_nonce, sig_esp_hex, g_device_id);

        // Minimal response: the app already holds sig_wallet and rebuilds `message` from
        // known fields. Bluefruit's notify() splits payloads above the MTU by itself
        // (BLECharacteristic.cpp:730), so this ~400 B object needs no manual chunking.
        char* resp = s_buf.reg.resp;
        snprintf(resp, sizeof(s_buf.reg.resp),
            "{\"status\":\"ok\",\"cmd\":\"register\","
            "\"sig_esp\":\"%s\",\"pubkey_esp\":\"%s\",\"proof\":\"%s\",\"ts\":%lu}",
            sig_esp_hex, pubkey_hex, proof_hex, millis() / 1000);
        notify(resp);

        // No restart and no BLE deinit here. Upstream restarts because it is handing the
        // shared radio from BLE to WiFi; this node has no WiFi and keeps BLE and LoRa up
        // together for its whole life (main.cpp:71/97), so tearing BLE down would strand
        // the app mid-onboarding and drop the LoRa uplink with it.
        LOGD("ble", "register ok — proof %.16s... (%d B resp)", proof_hex, (int)strlen(resp));
        return;
    }

    // trust_round {c} — one timing challenge-response round; src/ble_config.cpp:394-419.
    // r = sha256(c || device_id); rounds accumulate into the digest signed by trust_sign.
    // The reply must be fast (a bare sha256) — the app times it and the backend rejects
    // slow rounds as proxied.
    if (!strcmp(cmd, "trust_round")) {
        const char* c = doc["c"];
        if (!c || strlen(c) < 16 || strlen(c) > 64) { ble_err(cmd, "bad_challenge"); return; }
        if (s_rounds_count >= TRUST_MAX_ROUNDS)     { ble_err(cmd, "too_many_rounds"); return; }

        char r_hex[65];
        ceremony_round_hex(r_hex, s_buf.round.input, sizeof(s_buf.round.input), c, g_device_id);

        size_t used = strlen(s_rounds_buf);
        snprintf(s_rounds_buf + used, ROUNDS_BUF_LEN - used, "%s%s", c, r_hex);
        s_rounds_count++;

        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"trust_round\",\"r\":\"%s\",\"i\":%d}",
            r_hex, s_rounds_count);
        notify(resp);
        return;
    }

    // trust_sign {seed, owner, resume?, gps_lat?, gps_lon?} — src/ble_config.cpp:427-522.
    // The node signs the canonical attestation; app and backend rebuild the identical
    // string from the response fields, exactly as with register.
    if (!strcmp(cmd, "trust_sign")) {
        const char* seed  = doc["seed"];
        const char* owner = doc["owner"];
        bool        resume = doc["resume"] | false;
        (void)resume;   // accepted for payload parity; only a LOGD hint (see below)
        if (!seed || strlen(seed) < 16 || strlen(seed) > 64) { ble_err(cmd, "bad_seed"); return; }
        if (!owner || strlen(owner) < 20) { ble_err(cmd, "missing_owner"); return; }

        // Ceremony nonce — 8 B, separate from the session nonce issued by auth.
        uint8_t nrnd[8]; char nonce_hex[17];
        esp_fill_random(nrnd, 8);
        bytes_to_hex(nrnd, 8, nonce_hex);

        // Digest of the accumulated rounds ("-" when none were run).
        char rounds_hex[65];
        if (s_rounds_count > 0) {
            uint8_t rh[32];
            sha256_string(s_rounds_buf, rh);
            bytes_to_hex(rh, 32, rounds_hex);
        } else {
            strcpy(rounds_hex, "-");
        }

        // BLE address as seen over the air, plus the stable per-chip MAC the device_id
        // is derived from (esp_mac.h shim → FICR) — same pair upstream signs.
        char ble_mac[18];
        ble_mac_str(ble_mac);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char efuse_mac[13];
        bytes_to_hex(mac, 6, efuse_mac);

        unsigned long up = millis() / 1000;

        // GPS is optional and passed through VERBATIM as strings — never re-parsed as a
        // float, or the signature would diverge between firmware, app and backend.
        // Present → attestation v2, absent → v1.
        const char* gps_lat = doc["gps_lat"] | "";
        const char* gps_lon = doc["gps_lon"] | "";
        bool has_gps = strlen(gps_lat) > 0 && strlen(gps_lon) > 0 &&
                       strlen(gps_lat) < 16 && strlen(gps_lon) < 16;

        char* attest = s_buf.sign.attest;
        if (has_gps) {
            snprintf(attest, sizeof(s_buf.sign.attest),
                "{\"v\":2,\"device_id\":\"%s\",\"owner\":\"%s\","
                "\"seed\":\"%s\",\"nonce\":\"%s\",\"ble_mac\":\"%s\","
                "\"efuse_mac\":\"%s\",\"rounds\":\"%s\",\"uptime_s\":%lu,"
                "\"gps_lat\":\"%s\",\"gps_lon\":\"%s\"}",
                g_device_id, owner, seed, nonce_hex, ble_mac,
                efuse_mac, rounds_hex, up, gps_lat, gps_lon);
        } else {
            snprintf(attest, sizeof(s_buf.sign.attest),
                "{\"v\":1,\"device_id\":\"%s\",\"owner\":\"%s\","
                "\"seed\":\"%s\",\"nonce\":\"%s\",\"ble_mac\":\"%s\","
                "\"efuse_mac\":\"%s\",\"rounds\":\"%s\",\"uptime_s\":%lu}",
                g_device_id, owner, seed, nonce_hex, ble_mac,
                efuse_mac, rounds_hex, up);
        }

        uint8_t ah[32];
        sha256_string(attest, ah);
        uint8_t sig_raw[72]; size_t sig_len = 0;
        char* sig_hex = s_buf.sign.sig_hex; sig_hex[0] = 0;
        if (!identity_sign(ah, sig_raw, &sig_len)) { ble_err(cmd, "sign_failed"); return; }
        bytes_to_hex(sig_raw, sig_len, sig_hex);

        char* pubkey_hex = s_buf.sign.pubkey_hex;
        identity_get_pubkey_hex(pubkey_hex, sizeof(s_buf.sign.pubkey_hex));

        // Abbreviated keys — upstream shortened these to fit the response in one MTU 512
        // window; the app keys off these exact names.
        char* resp = s_buf.sign.resp;
        snprintf(resp, sizeof(s_buf.sign.resp),
            "{\"status\":\"ok\",\"cmd\":\"trust_sign\","
            "\"n\":\"%s\",\"bm\":\"%s\",\"em\":\"%s\",\"rd\":\"%s\","
            "\"up\":%lu,\"sig\":\"%s\",\"pk\":\"%s\",\"gv\":%d}",
            nonce_hex, ble_mac, efuse_mac, rounds_hex, up,
            sig_hex, pubkey_hex, has_gps ? 2 : 1);

        LOGD("ble", "trust_sign: %d rounds, resp %d B%s",
             s_rounds_count, (int)strlen(resp), resume ? " (resume)" : "");

        // One-shot ceremony — reset the rounds either way.
        s_rounds_buf[0] = '\0';
        s_rounds_count  = 0;

        // `resume` upstream also means "go back to WiFi"; here it is only a log hint,
        // since there is no radio handoff to make.
        notify(resp);
        return;
    }

    // wallet_status — does the node hold a wallet copy, and is it bound to an owner.
    // Upstream reports WiFi configuration in `configured`; with no WiFi on this board the
    // equivalent "onboarding finished" signal is having an owner bound.
    if (!strcmp(cmd, "wallet_status")) {
        Preferences p; p.begin(NVS_NS_WALLET, true);
        bool has = p.isKey("blob");
        String addr = p.getString("addr", "");
        p.end();
        char resp[180];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"cmd\":\"wallet_status\","
            "\"has_backup\":%s,\"configured\":%s,\"addr\":\"%s\"}",
            has ? "true" : "false",
            strlen(g_owner_address) > 0 ? "true" : "false",
            addr.c_str());
        notify(resp);
        return;
    }

    // wallet_backup {blob, addr} — store the encrypted copy opaquely (the app encrypts
    // it with the user's PIN; the node never sees plaintext).
    if (!strcmp(cmd, "wallet_backup")) {
        const char* blob = doc["blob"];
        const char* addr = doc["addr"] | "";
        if (!blob || strlen(blob) < 16 || strlen(blob) > 600) { ble_err(cmd, "bad_blob"); return; }
        Preferences p; p.begin(NVS_NS_WALLET, false);
        p.putString("blob", blob);
        p.putString("addr", addr);
        p.end();
        LOGI("ble", "wallet_backup saved (%d B, %.10s)", (int)strlen(blob), addr);
        ble_ok(cmd);
        return;
    }

    // wallet_restore — hand the encrypted copy back (decrypted in the app).
    // BLE + auth only, so it requires physical proximity.
    if (!strcmp(cmd, "wallet_restore")) {
        Preferences p; p.begin(NVS_NS_WALLET, true);
        String blob = p.getString("blob", "");
        String addr = p.getString("addr", "");
        p.end();
        if (blob.length() == 0) { ble_err(cmd, "no_backup"); return; }
        char* resp = s_buf.wallet.resp;
        snprintf(resp, sizeof(s_buf.wallet.resp),
            "{\"status\":\"ok\",\"cmd\":\"wallet_restore\","
            "\"blob\":\"%s\",\"addr\":\"%s\"}",
            blob.c_str(), addr.c_str());
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

// ── Boot self-test ────────────────────────────────────────────
// Pins the canonical ceremony strings to known-good values computed off-device from the
// ESP32 source formats. It calls the same helpers the live handlers call, so a format
// change cannot pass this without also changing the wire protocol.
bool ble_ceremony_selftest() {
    static const char DEV[] = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";
    static const char OWN[] = "0x1234567890abcdef1234567890abcdef12345678";
    static const char NON[] = "0011223344556677889900112233445566778899001122334455667788990011";
    static const char SIG[] = "3044022012345678901234567890123456789012345678901234567890123456789012340220"
                              "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
    static const char CHA[] = "0123456789abcdef0123456789abcdef";
    static const char EXP_MSG[] =
        "{\"device_id\":\"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90\","
        "\"owner\":\"0x1234567890abcdef1234567890abcdef12345678\","
        "\"nonce\":\"0011223344556677889900112233445566778899001122334455667788990011\",\"ts\":0}";
    static const char EXP_PROOF[] = "01d9ca7e58ff1fe067560ade32c4b9c9fa991499bd3a1a43cd69f7cf01bb6ded";
    static const char EXP_ROUND[] = "0b4d384f1e795444190ab14862bafe82cd835c036cd0c9b63c74d150119d015b";

    // Reuse the handler union — this runs from ble_load_config(), long before any BLE
    // command can be in flight, and keeps the fixtures off both the stack and .bss.
    char* msg = s_buf.reg.message;
    ceremony_register_msg(msg, sizeof(s_buf.reg.message), DEV, OWN, NON);
    // Compare before touching s_buf.round: round.input aliases reg.message at union
    // offset 0, so the round call below overwrites msg (the per-command safety contract
    // above does not hold inside this one function).
    bool msg_ok = !strcmp(msg, EXP_MSG);
    size_t msg_len = strlen(msg);

    char proof[65], r[65];
    ceremony_proof_hex(proof, s_buf.reg.proof_input, sizeof(s_buf.reg.proof_input), NON, SIG, DEV);
    ceremony_round_hex(r, s_buf.round.input, sizeof(s_buf.round.input), CHA, DEV);

    bool proof_ok = !strcmp(proof, EXP_PROOF);
    bool round_ok = !strcmp(r, EXP_ROUND);
    bool ok = msg_ok && proof_ok && round_ok;

    Serial.printf("[SELFTEST] ceremony=%s msg=%d proof=%d round=%d msg_len=%u\n",
                  ok ? "PASS" : "FAIL", (int)msg_ok, (int)proof_ok, (int)round_ok,
                  (unsigned)msg_len);
    if (!ok) {
        // Direct writes, not Serial.printf: the 215-byte message overruns printf's
        // vararg buffer and the tail comes out corrupted (see serial_cmd.cpp:123).
        Serial.print("[SELFTEST] ceremony msg  = "); Serial.println(msg);
        Serial.print("[SELFTEST] ceremony proof= "); Serial.println(proof);
        Serial.print("[SELFTEST] ceremony round= "); Serial.println(r);
        LOGE("ble", "ceremony self-test FAILED");
    }
    return ok;
}

// ── Lifecycle ─────────────────────────────────────────────────
static uint8_t s_uuid_svc[16], s_uuid_w[16], s_uuid_r[16];

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

void ble_start() {
    if (g_ble_active) return;
    if (!uuid128(BLE_SERVICE_UUID, s_uuid_svc) ||
        !uuid128(BLE_CHAR_WRITE_UUID, s_uuid_w) ||
        !uuid128(BLE_CHAR_READ_UUID, s_uuid_r)) {
        LOGE("ble", "uuid parse failed");
        return;
    }

    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);   // MTU 247 — framework ceiling
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
    // BOTH write callbacks take useAdaCallback=false, and that is load-bearing twice over.
    //
    // Single producer context: with the default (true) the framework defers the callback to
    // the "Callback" task (TASK_PRIO_NORMAL) while the authorize callback below runs on the
    // "BLE" task (TASK_PRIO_HIGH, bluefruit.cpp:473 — SD_EVT_IRQHandler only gives a
    // semaphore, it dispatches nothing). Two producer tasks on one unguarded 1-slot queue is
    // a data race: "BLE" preempts "Callback" at any instruction, so a long-write `register`
    // being copied could be torn in half by a short command arriving mid-memcpy, or silently
    // dropped as "busy". With false, both callbacks are invoked inline from
    // BLECharacteristic::_eventHandler on the "BLE" task, which drains events sequentially
    // and cannot preempt itself — the race is gone by construction rather than by locking,
    // so no critical section is needed in queue_cmd.
    //
    // Payload integrity: the deferred path copies only sizeof(ble_gatts_evt_write_t)
    // (BLECharacteristic.cpp:447), whose data[] is a 1-byte placeholder, so an authorized
    // write's JSON would arrive truncated to its first character. BLEDfu gets away with the
    // default because it only ever reads data[0].
    //
    // Both callbacks still do nothing but reply and copy; the JSON parse and the ECDSA
    // signature stay in ble_tick() on the loop task.
    s_char_w->setWriteCallback(on_write, false);
    // Enables BLE long write — see on_write_authorize. Must be installed before begin(),
    // which is what publishes attr_meta (and its wr_auth bit) to the SoftDevice.
    s_char_w->setWriteAuthorizeCallback(on_write_authorize, false);
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
