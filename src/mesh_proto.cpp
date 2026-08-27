#include "mesh_proto.h"
#if LORA_ENABLED

#include "log.h"
#include <mbedtls/aes.h>
#include <esp_mac.h>
#include <string.h>

// Default channel PSK (index 1) — meshtastic/firmware src/mesh/Channels.h:144.
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

// ── base64 decode (PSK strings arrive base64 like in the app) ─────────────────
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
static size_t b64_decode(const char* in, uint8_t* out, size_t out_cap) {
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ') continue;
        int v = b64_val(*p);
        if (v < 0) return 0;                       // not base64 — caller decides
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return 0;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

bool mesh_key_from_psk(const char* psk_b64, MeshChannelKey* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    // Empty / "AQ==" / "1" → PSK index 1, the default channel key.
    if (!psk_b64 || !psk_b64[0] || !strcmp(psk_b64, "1")) {
        memcpy(out->bytes, MESH_DEFAULT_PSK, sizeof(MESH_DEFAULT_PSK));
        out->len = sizeof(MESH_DEFAULT_PSK);
        return true;
    }

    uint8_t raw[MESH_MAX_KEY];
    size_t n = b64_decode(psk_b64, raw, sizeof(raw));
    if (n == 0) return false;

    if (n == 1) {
        // Index form: default key with the last byte advanced by (index - 1).
        // Index 0 means "no encryption" upstream; this port refuses it — a node
        // that ships plaintext sensor batches is worse than one that stays silent.
        if (raw[0] == 0) return false;
        memcpy(out->bytes, MESH_DEFAULT_PSK, sizeof(MESH_DEFAULT_PSK));
        out->bytes[sizeof(MESH_DEFAULT_PSK) - 1] += (uint8_t)(raw[0] - 1);
        out->len = sizeof(MESH_DEFAULT_PSK);
        return true;
    }
    if (n != 16 && n != 32) return false;          // AES-128 or AES-256 only
    memcpy(out->bytes, raw, n);
    out->len = (uint8_t)n;
    return true;
}

static uint8_t xor_hash(const uint8_t* p, size_t len) {
    uint8_t code = 0;
    for (size_t i = 0; i < len; i++) code ^= p[i];
    return code;
}

uint8_t mesh_channel_hash(const char* name, const MeshChannelKey& key) {
    uint8_t h = xor_hash((const uint8_t*)name, strlen(name));
    h ^= xor_hash(key.bytes, key.len);
    return h;
}

uint32_t mesh_node_num() {
    // The WiFi station MAC is the same silicon identifier identity.cpp folds into the
    // device id, so the mesh address and the SENSMOS id stay tied to one board. Folded
    // in the same shape as the nRF port folds its two FICR words: low half XOR high
    // half shifted a byte, so both ports mix every source byte into the result.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t lo = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                  ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    uint32_t hi = ((uint32_t)mac[0] << 8)  |  (uint32_t)mac[1];
    uint32_t n = lo ^ (hi << 8);
    if (n == 0 || n == MESH_BROADCAST_ADDR) n = 0x0BADC0DE;   // never collide with broadcast
    return n;
}

// ── Data protobuf (fields 1 and 2 only) ──────────────────────────────────────
// Wire-identical to what nanopb emits for `meshtastic.Data{portnum, payload}`:
// tag 0x08 varint, tag 0x12 length-delimited. Two fields do not justify pulling a
// protobuf generator into the build.
static size_t pb_varint(uint32_t v, uint8_t* out) {
    size_t n = 0;
    while (v >= 0x80) { out[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    out[n++] = (uint8_t)v;
    return n;
}

static size_t data_encode(const uint8_t* payload, size_t len, uint32_t portnum,
                          uint8_t* out, size_t cap) {
    size_t o = 0;
    if (cap < 2) return 0;
    out[o++] = 0x08;                                  // field 1 (portnum), varint
    o += pb_varint(portnum, out + o);
    if (o + 1 >= cap) return 0;
    out[o++] = 0x12;                                  // field 2 (payload), bytes
    o += pb_varint((uint32_t)len, out + o);
    if (o + len > cap) return 0;
    memcpy(out + o, payload, len);
    return o + len;
}

// ── AES-CTR (CryptoEngine::encryptAESCtr) ────────────────────────────────────
// Nonce: packetId as u64 LE (bytes 0-7), fromNode as u32 LE (bytes 8-11), zeros.
// Counter size 4 — the last four nonce bytes are the block counter.
//
// mbedtls_aes_crypt_ctr increments the WHOLE 128-bit block, upstream increments only
// the last four bytes. Identical here and not by luck: nonce bytes 12-15 start at zero
// and a frame is at most MESH_MAX_FRAME (255 B) = 16 blocks, so the counter never
// carries out of byte 15 into the fromNode field. A larger frame would diverge, which
// is why MESH_MAX_FRAME is the hard cap in mesh_frame_build.
static void mesh_ctr(const MeshChannelKey& key, uint32_t from_node, uint32_t packet_id,
                     uint8_t* buf, size_t len) {
    uint8_t nonce[16] = {0};
    uint64_t pid64 = (uint64_t)packet_id;
    memcpy(nonce, &pid64, sizeof(uint64_t));
    memcpy(nonce + sizeof(uint64_t), &from_node, sizeof(uint32_t));

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    // CTR encrypts with the FORWARD key schedule in both directions — setkey_dec here
    // would produce a keystream no Meshtastic node can reproduce.
    if (mbedtls_aes_setkey_enc(&ctx, key.bytes, (unsigned int)key.len * 8) == 0) {
        uint8_t stream[16] = {0};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, buf, buf);
    }
    mbedtls_aes_free(&ctx);
}

size_t mesh_frame_build(const uint8_t* payload, size_t payload_len, uint32_t portnum,
                        uint32_t packet_id, uint8_t channel_hash,
                        const MeshChannelKey& key, uint8_t hop_limit,
                        uint8_t* out, size_t out_cap) {
    if (!payload || !payload_len || !key.len) return 0;
    if (out_cap < MESH_HEADER_LEN + 8) return 0;
    if (out_cap > MESH_MAX_FRAME) out_cap = MESH_MAX_FRAME;   // keeps the CTR counter in byte 15

    size_t body = data_encode(payload, payload_len, portnum,
                              out + MESH_HEADER_LEN, out_cap - MESH_HEADER_LEN);
    if (!body) return 0;

    const uint32_t from = mesh_node_num();
    const uint32_t to   = MESH_BROADCAST_ADDR;
    if (hop_limit > 7) hop_limit = 7;
    // flags: hop_limit in bits 0-2, hop_start in bits 5-7 (want_ack and via_mqtt stay
    // clear — nothing acks a broadcast and this node is not an MQTT gateway).
    const uint8_t flags = (uint8_t)((hop_limit & 0x07) | ((hop_limit & 0x07) << 5));

    memcpy(out + 0,  &to,        4);
    memcpy(out + 4,  &from,      4);
    memcpy(out + 8,  &packet_id, 4);
    out[12] = flags;
    out[13] = channel_hash;
    out[14] = 0;                                       // next_hop  (unknown)
    out[15] = 0;                                       // relay_node (we are the origin)

    mesh_ctr(key, from, packet_id, out + MESH_HEADER_LEN, body);
    return MESH_HEADER_LEN + body;
}

bool mesh_proto_selftest() {
    const char* text = "SMOSB selftest 1/1";
    MeshChannelKey k;
    if (!mesh_key_from_psk("AQ==", &k) || k.len != 16 ||
        memcmp(k.bytes, MESH_DEFAULT_PSK, 16) != 0) {
        Serial.println("[SELFTEST] mesh=FAIL key");
        return false;
    }
    const uint8_t hash = mesh_channel_hash("LongFast", k);

    uint8_t frame[MESH_MAX_FRAME];
    size_t n = mesh_frame_build((const uint8_t*)text, strlen(text), MESH_PORT_TEXT,
                                0x12345678UL, hash, k, 3, frame, sizeof(frame));
    if (!n || n <= MESH_HEADER_LEN) {
        Serial.println("[SELFTEST] mesh=FAIL encode");
        return false;
    }

    // Header sanity: broadcast destination, our node as source.
    uint32_t to = 0, from = 0;
    memcpy(&to, frame, 4);
    memcpy(&from, frame + 4, 4);
    if (to != MESH_BROADCAST_ADDR || from != mesh_node_num()) {
        Serial.println("[SELFTEST] mesh=FAIL header");
        return false;
    }

    // CTR is its own inverse: re-running it must give back the plaintext protobuf,
    // which then has to start with the portnum tag and carry the text verbatim.
    size_t body = n - MESH_HEADER_LEN;
    mesh_ctr(k, from, 0x12345678UL, frame + MESH_HEADER_LEN, body);
    const uint8_t* pb = frame + MESH_HEADER_LEN;
    bool ok = (pb[0] == 0x08) && (pb[1] == MESH_PORT_TEXT) && (pb[2] == 0x12) &&
              (pb[3] == strlen(text)) &&
              (memcmp(pb + 4, text, strlen(text)) == 0);
    Serial.printf("[SELFTEST] mesh=%s frame=%uB node=0x%08lx chan_hash=0x%02x\n",
                  ok ? "PASS" : "FAIL", (unsigned)n, (unsigned long)from, hash);
    return ok;
}
#endif
