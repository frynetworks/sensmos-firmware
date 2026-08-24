// PrefsStore — Preferences-compatible key-value store on nRF52840 InternalFS
// (Adafruit LittleFS on internal flash). Same JSON-per-namespace format and
// crash-safe tmp+rename write path as the esp8266 port.
#include "prefs_store.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <ArduinoJson.h>

using namespace Adafruit_LittleFS_Namespace;

static bool s_fs_mounted = false;

static bool fs_ensure() {
    if (s_fs_mounted) return true;
    s_fs_mounted = InternalFS.begin();
    if (!s_fs_mounted) {                    // first boot: format once, retry
        InternalFS.format();
        s_fs_mounted = InternalFS.begin();
    }
    return s_fs_mounted;
}

// ── minimal base64 (libb64 replacement — not available on this core) ─────────
static const char B64A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t* in, size_t len, char* out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64A[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64A[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_decode(const char* in, size_t len, uint8_t* out) {
    int o = 0, acc = 0, nbits = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64_val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | v; nbits += 6;
        if (nbits >= 8) { nbits -= 8; out[o++] = (uint8_t)((acc >> nbits) & 0xFF); }
    }
    return o;
}

static JsonDocument& doc_of(void* p) { return *(JsonDocument*)p; }

bool PrefsStore::begin(const char* ns, bool readOnly) {
    if (_open) end();
    if (!fs_ensure()) return false;
    snprintf(_path, sizeof(_path), "/ns_%s.json", ns);
    _readOnly = readOnly;
    _doc = new JsonDocument();
    if (!_doc) return false;
    load();
    _open = true;
    _dirty = false;
    return true;
}

bool PrefsStore::load() {
    // Drop a stale <path>.tmp left by a write interrupted before its rename.
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", _path);
    if (InternalFS.exists(tmp)) InternalFS.remove(tmp);
    File f = InternalFS.open(_path, FILE_O_READ);
    if (!f) return false;
    DeserializationError e = deserializeJson(doc_of(_doc), f);
    f.close();
    if (e) doc_of(_doc).clear();            // corrupt file → defaults
    return !e;
}

bool PrefsStore::save() {
    if (_readOnly) return false;
    // Crash-safe write: serialize to <path>.tmp, close, then rename over <path>
    // (lfs rename is atomic). FILE_O_WRITE does not truncate on this core, so the
    // tmp is always removed first — it is guaranteed fresh.
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", _path);
    if (InternalFS.exists(tmp)) InternalFS.remove(tmp);
    File f = InternalFS.open(tmp, FILE_O_WRITE);
    if (!f) return false;
    serializeJson(doc_of(_doc), f);
    f.close();
    if (InternalFS.exists(_path)) InternalFS.remove(_path);
    if (!InternalFS.rename(tmp, _path)) {   // rename should not fail; if it does, drop the temp
        InternalFS.remove(tmp);
        return false;
    }
    _dirty = false;
    return true;
}

void PrefsStore::end() {
    if (!_open) return;
    if (_dirty) save();
    delete (JsonDocument*)_doc;
    _doc = nullptr;
    _open = false;
}

String PrefsStore::getString(const char* key, const String& defaultValue) {
    if (!_open) return defaultValue;
    JsonVariant v = doc_of(_doc)[key];
    if (v.is<const char*>()) return String(v.as<const char*>());
    return defaultValue;
}

size_t PrefsStore::getString(const char* key, char* value, size_t maxLen) {
    String s = getString(key, "");
    strlcpy(value, s.c_str(), maxLen);
    return strlen(value);
}

size_t PrefsStore::putString(const char* key, const char* value) {
    if (!_open || _readOnly) return 0;
    doc_of(_doc)[key] = value;              // ArduinoJson copies the string
    save();
    return strlen(value);
}

bool PrefsStore::getBool(const char* key, bool defaultValue) {
    if (!_open) return defaultValue;
    JsonVariant v = doc_of(_doc)[key];
    return v.is<bool>() ? v.as<bool>() : defaultValue;
}
size_t PrefsStore::putBool(const char* key, bool value) {
    if (!_open || _readOnly) return 0;
    doc_of(_doc)[key] = value; save(); return 1;
}

int32_t PrefsStore::getInt(const char* key, int32_t defaultValue) {
    if (!_open) return defaultValue;
    JsonVariant v = doc_of(_doc)[key];
    return v.is<int32_t>() ? v.as<int32_t>() : defaultValue;
}
size_t PrefsStore::putInt(const char* key, int32_t value) {
    if (!_open || _readOnly) return 0;
    doc_of(_doc)[key] = value; save(); return 4;
}

uint32_t PrefsStore::getUInt(const char* key, uint32_t defaultValue) {
    if (!_open) return defaultValue;
    JsonVariant v = doc_of(_doc)[key];
    return v.is<uint32_t>() ? v.as<uint32_t>() : defaultValue;
}
size_t PrefsStore::putUInt(const char* key, uint32_t value) {
    if (!_open || _readOnly) return 0;
    doc_of(_doc)[key] = value; save(); return 4;
}
uint32_t PrefsStore::getULong(const char* key, uint32_t defaultValue) { return getUInt(key, defaultValue); }
size_t   PrefsStore::putULong(const char* key, uint32_t value)        { return putUInt(key, value); }

uint8_t PrefsStore::getUChar(const char* key, uint8_t defaultValue) {
    return (uint8_t)getUInt(key, defaultValue);
}
size_t PrefsStore::putUChar(const char* key, uint8_t value) { return putUInt(key, value) ? 1 : 0; }

// Binary values: stored as "b64:<base64>" strings (round-trips privkey/pubkey exactly).
size_t PrefsStore::putBytes(const char* key, const void* value, size_t len) {
    if (!_open || _readOnly) return 0;
    size_t b64cap = ((len + 2) / 3) * 4 + 8;
    char* b64 = (char*)malloc(b64cap + 4);
    if (!b64) return 0;
    memcpy(b64, "b64:", 4);
    b64_encode((const uint8_t*)value, len, b64 + 4);
    doc_of(_doc)[key] = b64;
    free(b64);
    save();
    return len;
}

size_t PrefsStore::getBytes(const char* key, void* buf, size_t maxLen) {
    if (!_open) return 0;
    JsonVariant v = doc_of(_doc)[key];
    if (!v.is<const char*>()) return 0;
    const char* s = v.as<const char*>();
    if (strncmp(s, "b64:", 4) != 0) return 0;
    s += 4;
    size_t slen = strlen(s);
    uint8_t* tmp = (uint8_t*)malloc(slen + 4);
    if (!tmp) return 0;
    int n = b64_decode(s, slen, tmp);
    size_t out = (n < 0) ? 0 : ((size_t)n > maxLen ? maxLen : (size_t)n);
    if (out) memcpy(buf, tmp, out);
    free(tmp);
    return out;
}

size_t PrefsStore::getBytesLength(const char* key) {
    if (!_open) return 0;
    JsonVariant v = doc_of(_doc)[key];
    if (!v.is<const char*>()) return 0;
    const char* s = v.as<const char*>();
    if (strncmp(s, "b64:", 4) != 0) return 0;
    size_t slen = strlen(s + 4), pad = 0;
    if (slen >= 1 && s[4 + slen - 1] == '=') pad++;
    if (slen >= 2 && s[4 + slen - 2] == '=') pad++;
    return (slen / 4) * 3 - pad;
}

bool PrefsStore::isKey(const char* key) {
    if (!_open) return false;
    return !doc_of(_doc)[key].isNull();
}

bool PrefsStore::remove(const char* key) {
    if (!_open || _readOnly) return false;
    doc_of(_doc).remove(key);
    return save();
}

bool PrefsStore::clear() {
    if (!_open || _readOnly) return false;
    doc_of(_doc).clear();
    InternalFS.remove(_path);
    return true;
}
