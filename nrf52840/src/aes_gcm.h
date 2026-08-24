#pragma once
// AES-128-GCM wrapper — nRF52840 port. Same call shape as the esp8266 port's
// BearSSL gcm_frame(): in-place encrypt/decrypt with AAD + 16B tag, so the
// upstream session-frame layout [ver|seq|tag|ct] ports unchanged.
// Backend: rweather/Crypto GCM<AES128> (software; CryptoCell-310 evaluated and
// skipped for v1 — the BSP does not expose a GCM-capable CC310 API).
#include <stdint.h>
#include <stddef.h>

// encrypt=true:  data plaintext→ciphertext in place, tag written (16B).
// encrypt=false: data ciphertext→plaintext in place, tag verified; false = tag mismatch
//                (data content is then undefined — discard the frame).
bool aes128_gcm_frame(const uint8_t key[16], const uint8_t iv[12],
                      const uint8_t* aad, size_t aad_len,
                      uint8_t* data, size_t len,
                      uint8_t tag[16], bool encrypt);

// Boot self-test: round-trip + tamper detection. Prints "[SELFTEST] gcm=PASS/FAIL".
bool aes128_gcm_selftest();
