#pragma once
// Shim: ESP32 <Preferences.h> → InternalFS-backed PrefsStore (identical call surface).
// Lets upstream modules compile verbatim on nRF52840 (same trick as the esp8266 port).
#include "prefs_store.h"
using Preferences = PrefsStore;
