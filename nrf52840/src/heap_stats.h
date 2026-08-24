#pragma once
// Heap statistics for the Adafruit nRF52 core (newlib malloc on the app RAM region).
// free  = unused-by-malloc bytes still available to the application
//         (heap region not yet claimed via sbrk + free'd blocks inside the arena)
// max_block = best-effort largest contiguous allocation (bounded probe, no side effects)
#include <stdint.h>

struct HeapStats {
    uint32_t free_bytes;
    uint32_t max_block;
    uint32_t arena;        // bytes malloc has claimed from the region so far
    uint32_t region_total; // total bytes between heap base and stack limit
};

HeapStats heap_stats();
// Prints the canonical checkpoint line: "[HEAP] free=NNNNN max_block=NNNNN uptime=NNNs"
void heap_stats_print(const char* tag);
