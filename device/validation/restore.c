/* Same workload and ABI for both versions. Time store_get separately from the oracle. */
#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { PAGE = 4096, COUNT = 256, PASSES = 64 };
static uint8_t original[COUNT][PAGE];
static uint8_t restored[COUNT][PAGE];

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
int main(void) {
    struct store *store = store_new(COUNT);
    if (!store) return 1;
    for (unsigned p = 0; p < COUNT; p++) {
        for (unsigned i = 0; i < PAGE; i++) {
            unsigned lane = ((i % 64) * 23 + p) % 37 < 15;
            uint8_t left = p + i / 64;
            original[p][i] = lane ? (uint8_t)~left : left;
        }
        if (store_put(store, p, original[p])) return 2;
    }
    uint64_t models, before, after, total = 0;
    store_selector_stats(store, &models, &before);
    if (models != COUNT) return 3;
    for (unsigned pass = 0; pass <= PASSES; pass++) {
        uint64_t start = now_ns();
        for (unsigned i = 0; i < COUNT; i++) {
            unsigned p = (i * 73 + pass) % COUNT;
            if (store_get(store, p, restored[p])) return 4;
        }
        uint64_t elapsed = now_ns() - start;
        if (memcmp(original, restored, sizeof original)) return 5;
        if (pass) total += elapsed;
    }
    store_selector_stats(store, &models, &after);
    if (after - before != COUNT * (PASSES + 1)) return 6;
    printf("{\"verified_pages\":%u,\"timed_pages\":%u,\"cpu_ns\":%" PRIu64 "}\n",
           COUNT * (PASSES + 1), COUNT * PASSES, total);
    store_free(store);
    return 0;
}
