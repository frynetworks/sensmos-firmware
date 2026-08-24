#pragma once
#include <Arduino.h>

// SENSMOS logging — compact, one tag per module (same surface as the esp8266 port).
// Levels: E/W/I always on, D compiled out unless LOG_DEBUG=1.
// nRF52840: .rodata lives in flash natively — no PSTR gymnastics needed.

#ifndef LOG_DEBUG
#define LOG_DEBUG 0
#endif

#define LOGE(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#if LOG_DEBUG
#define LOGD(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define LOGD(tag, fmt, ...) do {} while (0)
#endif

void     log_heap_sample();   // call every loop() — tracks min largest-block (frag floor)
uint32_t log_heap_min();      // lowest largest-block seen since boot
void     log_health();        // one dense status line — call ~1/min
