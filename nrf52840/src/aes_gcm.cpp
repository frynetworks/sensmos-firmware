#include "aes_gcm.h"
#include "log.h"
#include <Arduino.h>
#include <AES.h>
#include <GCM.h>
#include <string.h>

bool aes128_gcm_frame(const uint8_t key[16], const uint8_t iv[12],
                      const uint8_t* aad, size_t aad_len,
                      uint8_t* data, size_t len,
                      uint8_t tag[16], bool encrypt) {
    GCM<AES128> gcm;
    if (!gcm.setKey(key, 16)) return false;
    if (!gcm.setIV(iv, 12)) return false;
    if (aad && aad_len) gcm.addAuthData(aad, aad_len);
    if (encrypt) {
        gcm.encrypt(data, data, len);
        gcm.computeTag(tag, 16);
        return true;
    }
    gcm.decrypt(data, data, len);
    return gcm.checkTag(tag, 16);
}

bool aes128_gcm_selftest() {
    const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                             0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    const uint8_t iv[12]  = {0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab};
    const uint8_t aad[10] = {1,2,3,4,5,6,7,8,9,10};
    const char* msg = "sensmos-nrf52840 gcm roundtrip";
    uint8_t buf[40] = {0};
    size_t n = strlen(msg);
    memcpy(buf, msg, n);
    uint8_t tag[16];

    if (!aes128_gcm_frame(key, iv, aad, sizeof(aad), buf, n, tag, true)) goto fail;
    if (memcmp(buf, msg, n) == 0) goto fail;                       // must actually encrypt
    if (!aes128_gcm_frame(key, iv, aad, sizeof(aad), buf, n, tag, false)) goto fail;
    if (memcmp(buf, msg, n) != 0) goto fail;                       // must round-trip

    // Tamper check: flip one ciphertext bit → tag must reject.
    if (!aes128_gcm_frame(key, iv, aad, sizeof(aad), buf, n, tag, true)) goto fail;
    buf[0] ^= 0x01;
    if (aes128_gcm_frame(key, iv, aad, sizeof(aad), buf, n, tag, false)) goto fail;

    Serial.println("[SELFTEST] gcm=PASS");
    return true;
fail:
    LOGE("gcm", "self-test FAILED");
    Serial.println("[SELFTEST] gcm=FAIL");
    return false;
}
