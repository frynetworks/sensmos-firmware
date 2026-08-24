# pre: extra_script — TYLKO env nodemcuv2_tls (patrz platformio.ini).
#
# arduinoWebSockets 2.7.3 twardo definiuje WEBSOCKETS_NETWORK_SSL_CLASS=WiFiClientSecure
# (WebSockets.h:213, bez #ifndef — -D z build_flags jest po cichu nadpisywane) i sam
# `new`-uje te klase (WebSocketsClient.cpp:262-263). Zeby wss:// szlo przez wolfSSL-owy
# WsTlsClient (esp8266/src/ws_tls.h), patchujemy KOPIE naglowka w .pio/libdeps/<env TLS>/:
# w galezi NETWORK_ESP8266 podmieniamy definicje SSL-klasy na warunkowa, wstrzykujac
# include naszego naglowka SCIEZKA BEZWZGLEDNA (kopia libdeps jest lokalna dla maszyny,
# nigdy nie commitowana; -Isrc odpada — src/ ma shimy WiFi.h/HTTPClient.h itd., ktore
# cieniowalyby naglowki bibliotek we wszystkich TU).
#
# Idempotentny. Env nodemcuv2 ma OSOBNA kopie libdeps — build shippingowy nietkniety.
# Reversal: usunac ten wpis z extra_scripts + skasowac .pio/libdeps/nodemcuv2_tls/WebSockets
# (pio odtworzy czysta kopie).

Import("env")
import os

MARKER = "WsTlsClient"

# Unikalna kotwica galezi NETWORK_ESP8266 (SSL-define wystepuje w 4 galeziach sieciowych).
ANCHOR = (
    "#include <ESP31BWiFi.h>\n"
    "#endif\n"
    "#define WEBSOCKETS_NETWORK_CLASS WiFiClient\n"
    "#define WEBSOCKETS_NETWORK_SSL_CLASS WiFiClientSecure\n"
)

def patch():
    lib_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env.subst("$PIOENV")
    ws_h = os.path.join(lib_dir, pioenv, "WebSockets", "src", "WebSockets.h")
    if not os.path.isfile(ws_h):
        print("[websockets-patch] SKIPPED (libdeps not installed yet): %s" % ws_h)
        return
    with open(ws_h, "r") as f:
        content = f.read()
    if MARKER in content:
        print("[websockets-patch] already patched: %s" % ws_h)
        return
    if ANCHOR not in content:
        print("[websockets-patch] ERROR: anchor not found in %s — NOT patched" % ws_h)
        return
    ws_tls_h = os.path.join(env.subst("$PROJECT_SRC_DIR"), "ws_tls.h").replace("\\", "/")
    replacement = (
        "#include <ESP31BWiFi.h>\n"
        "#endif\n"
        "#define WEBSOCKETS_NETWORK_CLASS WiFiClient\n"
        "#if defined(SENSMOS_USE_TLS)\n"
        "#include \"%s\"\n"
        "#define WEBSOCKETS_NETWORK_SSL_CLASS WsTlsClient\n"
        "#else\n"
        "#define WEBSOCKETS_NETWORK_SSL_CLASS WiFiClientSecure\n"
        "#endif\n"
    ) % ws_tls_h
    content = content.replace(ANCHOR, replacement, 1)
    with open(ws_h, "w") as f:
        f.write(content)
    print("[websockets-patch] patched %s (SSL class -> WsTlsClient under SENSMOS_USE_TLS)" % ws_h)

patch()
