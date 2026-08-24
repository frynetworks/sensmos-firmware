#include "heap_stats.h"
#include <Arduino.h>
#include <malloc.h>

// Nordic gcc startup/linker symbols (present in the Adafruit nRF52 core's ld scripts).
// Heap region = [__HeapBase, __HeapLimit); newlib sbrk grows inside it.
extern "C" char __HeapBase[];
extern "C" char __HeapLimit[];

static uint32_t region_total() {
    return (uint32_t)(__HeapLimit - __HeapBase);
}

// Largest contiguous block: bounded descending probe (alloc+free, ≤32 attempts).
// No lasting side effects — each candidate is freed immediately.
static uint32_t probe_max_block(uint32_t upper) {
    if (upper < 16) return 0;
    uint32_t lo = 0, hi = upper;
    for (int i = 0; i < 32 && hi - lo > 256; i++) {
        uint32_t mid = lo + (hi - lo) / 2;
        void* p = malloc(mid);
        if (p) { free(p); lo = mid; }
        else   { hi = mid; }
    }
    return lo;
}

HeapStats heap_stats() {
    struct mallinfo mi = mallinfo();
    HeapStats s;
    s.region_total = region_total();
    s.arena        = mi.arena;
    // Free = untouched region beyond the arena + free chunks inside the arena.
    uint32_t beyond = (s.region_total > mi.arena) ? (s.region_total - mi.arena) : 0;
    s.free_bytes   = beyond + mi.fordblks;
    s.max_block    = probe_max_block(s.free_bytes);
    return s;
}

void heap_stats_print(const char* tag) {
    HeapStats s = heap_stats();
    Serial.printf("[HEAP] free=%lu max_block=%lu uptime=%lus%s%s\n",
                  (unsigned long)s.free_bytes, (unsigned long)s.max_block,
                  (unsigned long)(millis() / 1000),
                  tag && tag[0] ? " tag=" : "", tag ? tag : "");
}
