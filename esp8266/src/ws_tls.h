#pragma once
// wolfSSL TLS PoC — entirely compiled out unless SENSMOS_USE_TLS is defined (nodemcuv2_tls
// PlatformIO env only). Measures on-device heap cost of a real TLS 1.3/1.2 handshake to
// produce actual numbers for the README's TLS-upgrade heap estimate. Does not touch the
// production WS protocol (ws_client.cpp) or its s_enc buffer invariant.
#ifdef SENSMOS_USE_TLS

// One-shot: connects to a public TLS test host, logs [tls-heap] readings at each stage
// (baseline/init/ctx/connect/handshake/steady-state/cleanup), then tears everything down.
// Call once from setup(), after Wi-Fi is up, before the normal WS connect loop starts.
void ws_tls_run_poc_test();

#endif // SENSMOS_USE_TLS
