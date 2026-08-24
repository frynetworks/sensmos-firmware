#include "log.h"
#include "heap_stats.h"

static uint32_t s_min_largest = 0xFFFFFFFF;

void log_heap_sample() {
    HeapStats s = heap_stats();
    if (s.max_block < s_min_largest) s_min_largest = s.max_block;
}

uint32_t log_heap_min() { return s_min_largest == 0xFFFFFFFF ? 0 : s_min_largest; }

void log_health() {
    HeapStats s = heap_stats();
    // free/largest now, min largest since boot (fragmentation floor).
    // Second line intentionally avoids the bare "heap <N>k" shape the heap gate greps.
    LOGI("health", "up=%lus heap=%luk blk=%luk min=%luk",
         (unsigned long)(millis() / 1000),
         (unsigned long)(s.free_bytes / 1024),
         (unsigned long)(s.max_block / 1024),
         (unsigned long)(log_heap_min() / 1024));
    LOGI("mem", "arena=%lu region=%lu",
         (unsigned long)s.arena, (unsigned long)s.region_total);
}
