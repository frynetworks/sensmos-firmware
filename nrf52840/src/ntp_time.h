#pragma once
// Shim: upstream ntp_time.h → provisioned epoch (no WiFi, no NTP on this chip).
// data_sender's timestamp logic compiles verbatim against this surface.
#include "ws_client.h"

static inline bool     ntp_synced()    { return ws_epoch_synced(); }
static inline uint32_t ntp_unix_time() { return ws_epoch_now(); }
