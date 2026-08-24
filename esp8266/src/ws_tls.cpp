#ifdef SENSMOS_USE_TLS
#include "ws_tls.h"
#include "log.h"
#include <Arduino.h>
#include <ESP8266WiFi.h>
#ifdef MMU_IRAM_HEAP
#include <umm_malloc/umm_heap_select.h>   // HeapSelectIram — patrz log.cpp/tunnel.cpp
#endif
#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>

// Trwały transport wss:// dla arduinoWebSockets (WsTlsClient) + jednorazowy PoC pomiarowy.
// CTX wolfSSL jest współdzielony i inicjalizowany RAZ (nigdy nie zwalniany) — biblioteka
// kasuje i tworzy instancję klienta przy każdym reconnect'cie (WebSocketsClient.cpp:252-263),
// sesje TLS są per-instancja.

static const unsigned long HS_DEADLINE_MS = 30000;   // twardy limit handshake'u (WANT_READ retry)

static WOLFSSL_CTX* g_ctx = nullptr;

// [tls-heap] line is DELIBERATELY not in the "heap <N>k" shape — see log.cpp's [mem] comment:
// tools/gate_heap.py's HEAP_RE would otherwise vacuum these readings into its min_heap gate.
// This transport only ever runs in the separate nodemcuv2_tls env, never the gated shipping
// build, but the naming discipline is kept so the two builds' logs stay distinguishable.
static void ws_tls_print_heap(const char* label) {
    uint32_t dram_free = ESP.getFreeHeap();
    uint32_t dram_blk  = ESP.getMaxFreeBlockSize();
    uint32_t frag       = ESP.getHeapFragmentation();
#ifdef MMU_IRAM_HEAP
    uint32_t iram_free = 0, iram_blk = 0;
    {
        HeapSelectIram ephemeral;
        iram_free = ESP.getFreeHeap();
        iram_blk  = ESP.getMaxFreeBlockSize();
    }
    LOGI("tls-heap", "%s dram_free=%u dram_blk=%u frag=%u%% iram_free=%u iram_blk=%u",
         label, (unsigned)dram_free, (unsigned)dram_blk, (unsigned)frag,
         (unsigned)iram_free, (unsigned)iram_blk);
#else
    LOGI("tls-heap", "%s dram_free=%u dram_blk=%u frag=%u%%",
         label, (unsigned)dram_free, (unsigned)dram_blk, (unsigned)frag);
#endif
}

// Custom wolfSSL I/O — ESP8266 Arduino's WiFiClient has no POSIX fd, so wolfSSL_set_fd()
// is not usable; wire wolfSSL's transport callbacks straight to the INHERITED WiFiClient.
// io_recv zwraca WANT_READ NATYCHMIAST gdy brak danych (bez 5s spinu jak w PoC) — czekanie
// dostarcza pętla retry w connect(); w steady-state loop() nie może być blokowany.
int ws_tls_io_send_cb(struct WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    WsTlsClient* self = (WsTlsClient*)ctx;
    size_t sent = self->WiFiClient::write((const uint8_t*)buf, (size_t)sz);
    if (sent == 0) return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return (int)sent;
}

int ws_tls_io_recv_cb(struct WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    WsTlsClient* self = (WsTlsClient*)ctx;
    if (self->WiFiClient::available() <= 0) {
        if (!self->WiFiClient::connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    int rd = self->WiFiClient::read((uint8_t*)buf, (size_t)sz);
    return rd > 0 ? rd : WOLFSSL_CBIO_ERR_WANT_READ;
}

static bool ws_tls_ctx_ensure() {
    if (g_ctx) return true;
    ws_tls_print_heap("baseline");
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        LOGE("tls", "wolfSSL_Init failed");
        return false;
    }
    ws_tls_print_heap("post-wolfssl-init");
    // v23 = TLS1.3-preferowany Z downgrade=1 (wolfTLSv1_3_client_method ma downgrade=0
    // i odrzuca ServerHello 1.2 jako err -326). NO_OLD_TLS przybija WOLFSSL_MIN_DOWNGRADE
    // do TLS1.2 — nizej nie zejdzie, bez zadnego SetMinVersion.
    g_ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!g_ctx) {
        LOGW("tls", "TLS1.3 CTX unavailable, falling back to TLS1.2");
        g_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    }
    if (!g_ctx) {
        LOGE("tls", "CTX_new failed (1.3 and 1.2 both unavailable)");
        wolfSSL_Cleanup();
        return false;
    }
    // Bez weryfikacji certu (postawa PoC — pinning jak w identify/ECDH to follow-up).
    wolfSSL_CTX_set_verify(g_ctx, WOLFSSL_VERIFY_NONE, nullptr);
    wolfSSL_CTX_SetIOSend(g_ctx, ws_tls_io_send_cb);
    wolfSSL_CTX_SetIORecv(g_ctx, ws_tls_io_recv_cb);
    ws_tls_print_heap("post-ctx-new");
    return true;
}

WsTlsClient::~WsTlsClient() {
    WsTlsClient::stop();
}

int WsTlsClient::connect(const char* host, uint16_t port) {
    // DNS tutaj — bazowe WiFiClient::connect(host,...) woła WIRTUALNE connect(IPAddress,...)
    // i wpadłoby z powrotem w nasz override (nieskończona rekursja).
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) {
        LOGE("tls", "DNS resolve failed for %s", host);
        return 0;
    }
    return tlsConnect(ip, port, host);
}

int WsTlsClient::connect(IPAddress ip, uint16_t port) {
    return tlsConnect(ip, port, nullptr);   // bez SNI — lib łączy po hoście, ta ścieżka to komplet interfejsu
}

int WsTlsClient::tlsConnect(IPAddress ip, uint16_t port, const char* sniHost) {
    if (!ws_tls_ctx_ensure()) return 0;
    ws_tls_print_heap("pre-connect");

    if (!WiFiClient::connect(ip, port)) {   // kwalifikowane = nie-wirtualne, prosto w bazę
        LOGE("tls", "TCP connect to %s:%u failed",
             sniHost ? sniHost : ip.toString().c_str(), (unsigned)port);
        return 0;
    }
    ws_tls_print_heap("post-tcp-connect");

    _ssl = wolfSSL_new(g_ctx);
    if (!_ssl) {
        LOGE("tls", "SSL_new failed");
        WiFiClient::stop();
        return 0;
    }
    wolfSSL_SetIOReadCtx(_ssl, this);
    wolfSSL_SetIOWriteCtx(_ssl, this);
    if (sniHost)
        wolfSSL_UseSNI(_ssl, WOLFSSL_SNI_HOST_NAME, sniHost, (word16)strlen(sniHost));
    ws_tls_print_heap("post-ssl-new");

    // Handshake z ograniczonym retry na WANT_READ/WANT_WRITE — naprawia jednostrzałowy
    // wolfSSL_connect z PoC (io_recv po 5s idle ubijał handshake na stałe). delay(5)+yield()
    // karmią SW+HW WDT (patrz main.cpp: ESP.wdtEnable(8000)); jedyny koszt to kosmetyczny
    // "[loop] slow pass" przy reconnect'cie.
    LOGI("tls", "handshake starting: %s:%u", sniHost ? sniHost : "(ip)", (unsigned)port);
    unsigned long deadline = millis() + HS_DEADLINE_MS;
    int ret;
    for (;;) {
        ret = wolfSSL_connect(_ssl);
        if (ret == WOLFSSL_SUCCESS) break;
        int err = wolfSSL_get_error(_ssl, ret);
        if ((err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) &&
            (long)(deadline - millis()) > 0) {
            delay(5);
            yield();
            continue;
        }
        char errBuf[WOLFSSL_MAX_ERROR_SZ];
        wolfSSL_ERR_error_string(err, errBuf);
        LOGE("tls", "handshake failed: err=%d (%s)", err, errBuf);
        ws_tls_print_heap("post-handshake-fail");
        wolfSSL_free(_ssl); _ssl = nullptr;
        WiFiClient::stop();
        return 0;
    }

    _hsDone = true;
    ws_tls_print_heap("post-handshake-ok");
    LOGI("tls", "connected: version=%s cipher=%s",
         wolfSSL_get_version(_ssl), wolfSSL_get_cipher(_ssl));
    return 1;
}

size_t WsTlsClient::write(uint8_t b) {
    return write(&b, 1);
}

size_t WsTlsClient::write(const uint8_t* buf, size_t size) {
    if (!_ssl || size == 0) return 0;
    int w = wolfSSL_write(_ssl, buf, (int)size);
    if (w > 0) return (size_t)w;
    // WANT_WRITE (bufor TX pełny) => 0 — pętla zapisu biblioteki retry'uje z timeoutem 5s
    // (WebSockets.cpp:690-710). Błąd fatalny też daje 0; connected() zaraz zgłosi zgon.
    return 0;
}

int WsTlsClient::available() {
    if (!_ssl) return 0;
    int p = wolfSSL_pending(_ssl);
    if (p > 0) return p;
    if (WiFiClient::available() > 0) {
        // Pompka: przetwórz zalegle rekordy TLS (nieblokująco — io_recv zwraca WANT_READ
        // natychmiast przy niekompletnym rekordzie; stan parsera trzyma obiekt _ssl).
        uint8_t b;
        if (wolfSSL_peek(_ssl, &b, 1) > 0) return wolfSSL_pending(_ssl);
    }
    return 0;
}

int WsTlsClient::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? (int)b : -1;
}

int WsTlsClient::read(uint8_t* buf, size_t size) {
    if (!_ssl || size == 0) return 0;
    int r = wolfSSL_read(_ssl, buf, (int)size);
    return r > 0 ? r : 0;
}

int WsTlsClient::peek() {
    if (!_ssl) return -1;
    uint8_t b;
    return wolfSSL_peek(_ssl, &b, 1) > 0 ? (int)b : -1;
}

void WsTlsClient::flush() {
    // wolfSSL_write pisze rekordy na wylot — na poziomie TLS nie ma nic do spłukania.
    WiFiClient::flush();
}

uint8_t WsTlsClient::connected() {
    if (_ssl && wolfSSL_pending(_ssl) > 0) return 1;   // zbuforowany plaintext do wyczytania
    return (_ssl != nullptr && WiFiClient::connected()) ? 1 : 0;
}

void WsTlsClient::stop() {
    if (_ssl) {
        wolfSSL_shutdown(_ssl);   // pojedyncza, nieblokująca próba close_notify
        wolfSSL_free(_ssl);
        _ssl = nullptr;
        if (_hsDone) {
            _hsDone = false;
            ws_tls_print_heap("post-disconnect");
        }
    }
    WiFiClient::stop();
}

// ── Jednorazowy PoC pomiarowy (diagnostyka; nieużywany w obrazie -> gc-sections wytnie) ──
void ws_tls_run_poc_test() {
    LOGI("tls", "=== wolfSSL TLS PoC start ===");
    {
        WsTlsClient c;
        if (c.connect("httpbin.org", 443)) {
            ws_tls_print_heap("steady-state");
            const char* req = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
            c.write((const uint8_t*)req, strlen(req));
            unsigned long deadline = millis() + 5000;
            char resp[192];
            int rd = 0;
            while ((long)(deadline - millis()) > 0 && rd <= 0) {
                if (c.available() > 0) rd = c.read((uint8_t*)resp, sizeof(resp) - 1);
                else { delay(5); yield(); }
            }
            if (rd > 0) {
                resp[rd] = '\0';
                LOGI("tls", "response (%d bytes, truncated): %.100s", rd, resp);
            } else {
                LOGW("tls", "no response body read (rd=%d)", rd);
            }
            c.stop();
        } else {
            LOGE("tls", "PoC handshake did not complete");
        }
    }
    ws_tls_print_heap("post-cleanup");
    LOGI("tls", "=== wolfSSL TLS PoC end ===");
}

#endif // SENSMOS_USE_TLS
