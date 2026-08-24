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

// Public test host for the PoC handshake — no relation to the production WS backend.
// Chosen for reliable TLS 1.3/1.2 availability from arbitrary networks.
static const char* TLS_POC_HOST = "httpbin.org";
static const uint16_t TLS_POC_PORT = 443;
static const unsigned long IO_WAIT_MS = 5000;

static WOLFSSL_CTX* g_ctx = nullptr;
static WOLFSSL*     g_ssl = nullptr;

// [tls-heap] line is DELIBERATELY not in the "heap <N>k" shape — see log.cpp's [mem] comment:
// tools/gate_heap.py's HEAP_RE would otherwise vacuum these readings into its min_heap gate.
// This PoC only ever runs in the separate nodemcuv2_tls env, never the gated shipping build,
// but the naming discipline is kept anyway so the two builds' logs stay safely distinguishable.
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
// is not usable; wire wolfSSL's transport callbacks straight to WiFiClient read()/write().
static int ws_tls_io_send(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    WiFiClient* tcp = (WiFiClient*)ctx;
    size_t sent = tcp->write((const uint8_t*)buf, (size_t)sz);
    if (sent == 0) return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return (int)sent;
}

static int ws_tls_io_recv(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    WiFiClient* tcp = (WiFiClient*)ctx;
    unsigned long start = millis();
    while (tcp->available() <= 0) {
        if (!tcp->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
        if (millis() - start > IO_WAIT_MS) return WOLFSSL_CBIO_ERR_WANT_READ;
        delay(5);
        yield();   // critical on the NONOS SDK — let the WiFi stack service the socket
    }
    int rd = tcp->read((uint8_t*)buf, (size_t)sz);
    return rd > 0 ? rd : WOLFSSL_CBIO_ERR_WANT_READ;
}

static bool ws_tls_init() {
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
    ws_tls_print_heap("post-ctx-new");

    // PoC only — no cert pinning/verification. A production integration would pin the
    // backend's certificate instead of accepting anything, same intent as the identify/
    // ECDH handshake already does for the WS protocol.
    wolfSSL_CTX_set_verify(g_ctx, WOLFSSL_VERIFY_NONE, nullptr);
    wolfSSL_CTX_SetIOSend(g_ctx, ws_tls_io_send);
    wolfSSL_CTX_SetIORecv(g_ctx, ws_tls_io_recv);
    ws_tls_print_heap("post-config");
    return true;
}

static void ws_tls_cleanup() {
    if (g_ssl) { wolfSSL_free(g_ssl); g_ssl = nullptr; }
    if (g_ctx) { wolfSSL_CTX_free(g_ctx); g_ctx = nullptr; }
    wolfSSL_Cleanup();
    ws_tls_print_heap("post-cleanup");
}

static bool ws_tls_connect(WiFiClient& tcp, const char* host, uint16_t port) {
    ws_tls_print_heap("pre-connect");

    if (!tcp.connect(host, port)) {
        LOGE("tls", "TCP connect to %s:%u failed", host, (unsigned)port);
        return false;
    }
    ws_tls_print_heap("post-tcp-connect");

    g_ssl = wolfSSL_new(g_ctx);
    if (!g_ssl) {
        LOGE("tls", "SSL_new failed");
        tcp.stop();
        return false;
    }
    wolfSSL_SetIOReadCtx(g_ssl, &tcp);
    wolfSSL_SetIOWriteCtx(g_ssl, &tcp);
    wolfSSL_UseSNI(g_ssl, WOLFSSL_SNI_HOST_NAME, host, (word16)strlen(host));
    ws_tls_print_heap("post-ssl-new");

    LOGI("tls", "handshake starting: %s:%u", host, (unsigned)port);
    int ret = wolfSSL_connect(g_ssl);
    if (ret != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(g_ssl, ret);
        char errBuf[WOLFSSL_MAX_ERROR_SZ];
        wolfSSL_ERR_error_string(err, errBuf);
        LOGE("tls", "handshake failed: err=%d (%s)", err, errBuf);
        ws_tls_print_heap("post-handshake-fail");
        wolfSSL_free(g_ssl); g_ssl = nullptr;
        tcp.stop();
        return false;
    }

    ws_tls_print_heap("post-handshake-ok");
    LOGI("tls", "connected: version=%s cipher=%s",
         wolfSSL_get_version(g_ssl), wolfSSL_get_cipher(g_ssl));
    return true;
}

static void ws_tls_disconnect(WiFiClient& tcp) {
    if (g_ssl) {
        wolfSSL_shutdown(g_ssl);
        wolfSSL_free(g_ssl);
        g_ssl = nullptr;
    }
    tcp.stop();
    ws_tls_print_heap("post-disconnect");
}

void ws_tls_run_poc_test() {
    LOGI("tls", "=== wolfSSL TLS PoC start ===");

    if (!ws_tls_init()) {
        LOGE("tls", "PoC aborted: init failed");
        ws_tls_cleanup();
        return;
    }

    WiFiClient tcp;
    if (ws_tls_connect(tcp, TLS_POC_HOST, TLS_POC_PORT)) {
        ws_tls_print_heap("steady-state");

        char req[128];
        int n = snprintf(req, sizeof(req),
                          "GET /get HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                          TLS_POC_HOST);
        wolfSSL_write(g_ssl, req, n);

        char resp[192];
        int rd = wolfSSL_read(g_ssl, resp, sizeof(resp) - 1);
        if (rd > 0) {
            resp[rd] = '\0';
            LOGI("tls", "response (%d bytes, truncated): %.100s", rd, resp);
        } else {
            LOGW("tls", "no response body read (rd=%d)", rd);
        }

        ws_tls_disconnect(tcp);
    } else {
        LOGE("tls", "PoC handshake did not complete");
    }

    ws_tls_cleanup();
    LOGI("tls", "=== wolfSSL TLS PoC end ===");
}

#endif // SENSMOS_USE_TLS
