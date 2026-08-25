#include "mesh_tx.h"
#if LORA_ENABLED

#include "mesh_proto.h"
#include "identity.h"
#include "log.h"
#include "esp_random.h"
#include <Preferences.h>
#include <string.h>

// ── Config (persisted in the "sensmos_mesh" namespace) ───────────────────────
static bool           s_enabled  = MESH_TX_DEFAULT;
static char           s_channel[24] = MESH_CHANNEL_DEFAULT;
static char           s_psk[48]     = MESH_PSK_DEFAULT;
static uint32_t       s_portnum     = MESH_PORT_TEXT;
static MeshChannelKey s_key;
static uint8_t        s_chan_hash = 0;

// ── Uplink queue (one batch in flight, chunked) ──────────────────────────────
static char     s_buf[MESH_UPLINK_BUF];
static size_t   s_len   = 0;
static uint16_t s_seq   = 0;
static uint8_t  s_frag  = 0, s_nfrag = 0;
static bool     s_pending = false;
static uint32_t s_packet_id = 0;
static uint32_t s_sent_total = 0;
static uint32_t s_air_total  = 0;

static bool apply_psk() {
    MeshChannelKey k;
    if (!mesh_key_from_psk(s_psk, &k)) return false;
    s_key = k;
    s_chan_hash = mesh_channel_hash(s_channel, s_key);
    return true;
}

void mesh_tx_init() {
    Preferences p;
    if (p.begin("sensmos_mesh", true)) {
        s_enabled = p.getBool("enabled", MESH_TX_DEFAULT);
        String ch = p.getString("channel", "");
        String pk = p.getString("psk", "");
        uint32_t pn = p.getUInt("portnum", MESH_PORT_TEXT);
        if (ch.length()) ch.toCharArray(s_channel, sizeof(s_channel));
        if (pk.length()) pk.toCharArray(s_psk, sizeof(s_psk));
        s_portnum = pn;
        p.end();
    }
    if (!apply_psk()) {
        // Stored PSK unusable (hand-edited file, truncated write): fall back to the
        // default channel rather than transmitting with a key nothing can decrypt.
        LOGW("mesh", "stored psk invalid — reverting to default channel key");
        strncpy(s_psk, MESH_PSK_DEFAULT, sizeof(s_psk) - 1);
        apply_psk();
    }
    // Packet ids must not repeat under one key: the CTR nonce is built from them.
    // Seed from the TRNG so a reboot cannot replay the previous run's ids.
    esp_fill_random((uint8_t*)&s_packet_id, sizeof(s_packet_id));
    if (!s_packet_id) s_packet_id = 1;

    LOGI("mesh", "%s — channel \"%s\" (%s, hash 0x%02x), port %lu, node 0x%08lx",
         s_enabled ? "enabled" : "disabled", s_channel,
         s_key.len == 16 ? "AES-128-CTR" : "AES-256-CTR", s_chan_hash,
         (unsigned long)s_portnum, (unsigned long)mesh_node_num());
}

bool mesh_tx_enabled()   { return s_enabled; }
bool mesh_uplink_pending() { return s_pending; }

bool mesh_tx_configure(bool enabled, const char* channel, const char* psk_b64,
                       uint32_t portnum) {
    char old_ch[sizeof(s_channel)], old_psk[sizeof(s_psk)];
    strncpy(old_ch, s_channel, sizeof(old_ch));
    strncpy(old_psk, s_psk, sizeof(old_psk));

    if (channel && channel[0]) strncpy(s_channel, channel, sizeof(s_channel) - 1);
    if (psk_b64)               strncpy(s_psk, psk_b64, sizeof(s_psk) - 1);
    if (!apply_psk()) {                       // bad PSK: nothing changes
        strncpy(s_channel, old_ch, sizeof(s_channel));
        strncpy(s_psk, old_psk, sizeof(s_psk));
        apply_psk();
        return false;
    }
    s_enabled = enabled;
    if (portnum) s_portnum = portnum;

    Preferences p;
    if (p.begin("sensmos_mesh", false)) {
        p.putBool("enabled", s_enabled);
        p.putString("channel", s_channel);
        p.putString("psk", s_psk);
        p.putUInt("portnum", s_portnum);
        p.end();
    }
    LOGI("mesh", "config: %s channel \"%s\" (%s, hash 0x%02x) port %lu",
         s_enabled ? "on" : "off", s_channel,
         s_key.len == 16 ? "AES-128-CTR" : "AES-256-CTR", s_chan_hash,
         (unsigned long)s_portnum);
    return true;
}

void mesh_tx_status_json(String& out) {
    char b[240];
    snprintf(b, sizeof(b),
        "{\"enabled\":%s,\"channel\":\"%s\",\"cipher\":\"%s\",\"chan_hash\":%u,"
        "\"portnum\":%lu,\"node_num\":%lu,\"freq\":%.3f,\"sf\":%u,\"bw\":%.1f,"
        "\"pending\":%s,\"packets\":%lu,\"air_ms\":%lu}",
        s_enabled ? "true" : "false", s_channel,
        s_key.len == 16 ? "aes128-ctr" : "aes256-ctr", s_chan_hash,
        (unsigned long)s_portnum, (unsigned long)mesh_node_num(),
        MESH_FREQ, MESH_SF, MESH_BW, s_pending ? "true" : "false",
        (unsigned long)s_sent_total, (unsigned long)s_air_total);
    out = b;
}

bool mesh_uplink_enqueue(const char* json, size_t len) {
    if (!s_enabled || !len || len >= MESH_UPLINK_BUF) return false;
    if (s_pending) return false;
    memcpy(s_buf, json, len);
    s_len   = len;
    s_frag  = 0;
    s_nfrag = (uint8_t)((len + MESH_CHUNK_RAW - 1) / MESH_CHUNK_RAW);
    s_pending = true;
    LOGI("mesh", "uplink queued: %uB -> %u packets (seq %u)",
         (unsigned)len, s_nfrag, s_seq);
    return true;
}

// Airtime per the SX126x datasheet formula — same helper as the SMOS path, kept
// local so mesh TX can be budget-checked before the radio is retuned.
static uint32_t mesh_airtime_ms(uint8_t sf, float bw_khz, uint8_t cr, uint16_t len) {
    if (sf < 6)  sf = 6;
    if (sf > 12) sf = 12;
    if (cr < 5)  cr = 5;
    if (bw_khz <= 0) bw_khz = 250.0f;
    const float ts  = (float)(1UL << sf) / bw_khz;
    const int   de  = (ts > 16.0f) ? 1 : 0;
    const int   num = 8 * (int)len - 4 * (int)sf + 28 + 16;
    const int   den = 4 * ((int)sf - 2 * de);
    const int   n   = 8 + (num > 0 ? ((num + den - 1) / den) * (int)cr : 0);
    return (uint32_t)((MESH_PREAMBLE + 4.25f + n) * ts) + 1;
}

uint32_t mesh_tx_next(SX1262& radio, uint32_t duty_ms, uint32_t duty_budget,
                      float tcxo, int8_t rxen_pin, bool dio2_rf) {
    if (!s_pending || !s_enabled) return 0;

    // Chunk framing mirrors the SMOSB LoRa frames, minus the 0xE1 magic: as a text
    // packet it stays human-readable in a Meshtastic client, which is what makes
    // "did the mesh actually carry it" observable at all.
    size_t off = (size_t)s_frag * MESH_CHUNK_RAW;
    size_t raw = s_len - off;
    if (raw > MESH_CHUNK_RAW) raw = MESH_CHUNK_RAW;

    char text[MESH_CHUNK_RAW + 48];
    int tn = snprintf(text, sizeof(text), "SMOSB %.8s %04x %u/%u %.*s",
                      g_device_id, (unsigned)s_seq,
                      (unsigned)(s_frag + 1), (unsigned)s_nfrag,
                      (int)raw, s_buf + off);
    if (tn <= 0) { s_pending = false; return 0; }
    if (tn > (int)sizeof(text) - 1) tn = (int)sizeof(text) - 1;

    uint8_t frame[MESH_MAX_FRAME];
    size_t flen = mesh_frame_build((const uint8_t*)text, (size_t)tn, s_portnum,
                                   s_packet_id, s_chan_hash, s_key,
                                   MESH_HOP_LIMIT, frame, sizeof(frame));
    if (!flen) { LOGW("mesh", "frame build failed — dropping batch"); s_pending = false; return 0; }

    const uint32_t est = mesh_airtime_ms(MESH_SF, MESH_BW, MESH_CR, (uint16_t)flen);
    if (duty_ms + est > duty_budget) {
        // The caller passes g4's own counter and limit — mesh waits only when the
        // Meshtastic sub-band itself is spent, never because SMOS used up g1.
        // Log once per duty window — the counter falling is the caller starting a
        // new hour, which is the only moment a fresh warning carries information.
        static uint32_t last_duty = 0;
        static bool     warned    = false;
        if (duty_ms < last_duty) warned = false;
        last_duty = duty_ms;
        if (!warned) {
            warned = true;
            LOGW("mesh", "TX paused — g4 duty budget spent (%lu/%lums per h, need %lums)",
                 (unsigned long)duty_ms, (unsigned long)duty_budget, (unsigned long)est);
        }
        return 0;
    }

    // Retune to the Meshtastic channel. begin() resets the module, so the RF-switch
    // wiring has to be re-applied here exactly like lora_scan's after_begin().
    int st = radio.begin(MESH_FREQ, MESH_BW, MESH_SF, MESH_CR, MESH_SYNCWORD,
                         MESH_TX_POWER, MESH_PREAMBLE, tcxo, false);
    if (st != RADIOLIB_ERR_NONE) {
        LOGW("mesh", "radio switch to Meshtastic failed (%d)", st);
        return 0;
    }
    if (rxen_pin >= 0) radio.setRfSwitchPins(rxen_pin, RADIOLIB_NC);
    if (dio2_rf)       radio.setDio2AsRfSwitch(true);
    radio.setOutputPower(MESH_TX_POWER);
    LOGI("mesh", "radio -> %.3f MHz SF%u BW%.0f sync 0x%02X preamble %u",
         MESH_FREQ, MESH_SF, MESH_BW, MESH_SYNCWORD, MESH_PREAMBLE);

    const uint32_t t0 = millis();
    st = radio.transmit(frame, flen);
    const uint32_t air = millis() - t0;
    if (st != RADIOLIB_ERR_NONE) {
        LOGW("mesh", "TX failed (%d)", st);
        return 0;                                  // caller still restores SMOS config
    }

    s_sent_total++;
    s_air_total += air;
    LOGI("mesh", "packet %u/%u sent @%.3f SF%u (id 0x%08lx, %uB, %lums air, duty %lu/%lums/h g4)",
         (unsigned)(s_frag + 1), (unsigned)s_nfrag, MESH_FREQ, MESH_SF,
         (unsigned long)s_packet_id, (unsigned)flen,
         (unsigned long)air, (unsigned long)(duty_ms + air), (unsigned long)duty_budget);
    s_packet_id++;
    if (++s_frag >= s_nfrag) {
        s_pending = false;
        LOGI("mesh", "uplink batch seq %u complete (%u packets)",
             (unsigned)s_seq, (unsigned)s_nfrag);
        s_seq++;
    }
    return air;
}
#endif
