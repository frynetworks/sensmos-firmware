#pragma once
#include <Arduino.h>

// ══════════════════════════════════════════════════════════════
// Meshtastic wire format — encoder only (this node never receives mesh traffic).
//
// A Meshtastic LoRa frame is a 16-byte plaintext header followed by the AES-CTR
// encrypted `Data` protobuf. Field layout, nonce construction, key derivation and
// the channel hash below all mirror meshtastic/firmware master:
//   src/mesh/RadioInterface.h   PacketHeader + PACKET_FLAGS_* masks
//   src/mesh/CryptoEngine.cpp   initNonce() / encryptAESCtr() (CTR counter size 4)
//   src/mesh/Channels.h/.cpp    defaultpsk, PSK-index expansion, xorHash channel hash
// Getting any of these wrong produces a frame real nodes silently drop, so they are
// transcribed rather than approximated.
// ══════════════════════════════════════════════════════════════

#define MESH_HEADER_LEN     16
#define MESH_BROADCAST_ADDR 0xFFFFFFFFUL
#define MESH_MAX_KEY        32
#define MESH_MAX_FRAME      255      // Semtech max LoRa payload

// Data protobuf: field 1 = PortNum (varint), field 2 = payload (bytes).
#define MESH_PORT_TEXT      1        // TEXT_MESSAGE_APP — visible in any Meshtastic client
#define MESH_PORT_PRIVATE   256      // PRIVATE_APP

struct MeshChannelKey {
    uint8_t bytes[MESH_MAX_KEY];
    uint8_t len;                     // 16 = AES-128-CTR, 32 = AES-256-CTR
};

// PSK forms accepted, same as the Meshtastic app:
//   ""/"AQ=="/"1"  → PSK index 1 = the well-known default ("LongFast") key
//   base64 16 B    → AES-128 key verbatim
//   base64 32 B    → AES-256 key verbatim
// A single-byte PSK is the index form: default key with its last byte + (index-1).
bool mesh_key_from_psk(const char* psk_b64, MeshChannelKey* out);

// Channel hash byte carried in the header: xorHash(name) ^ xorHash(key bytes).
uint8_t mesh_channel_hash(const char* name, const MeshChannelKey& key);

// This node's Meshtastic NodeNum, derived from the nRF FICR device address
// (upstream derives it from the MAC — same idea, same stability across reboots).
uint32_t mesh_node_num();

// Encode payload as a Data protobuf, encrypt it, and prepend the header.
// Returns the frame length written to `out`, or 0 on error.
size_t mesh_frame_build(const uint8_t* payload, size_t payload_len, uint32_t portnum,
                        uint32_t packet_id, uint8_t channel_hash,
                        const MeshChannelKey& key, uint8_t hop_limit,
                        uint8_t* out, size_t out_cap);

// Boot self-test: protobuf encoding, key derivation, CTR round-trip (encrypt then
// decrypt must return the plaintext, since CTR is its own inverse).
// Prints "[SELFTEST] mesh=PASS|FAIL ..." — greppable like the other self-tests.
bool mesh_proto_selftest();
