/* Replay the same mixed overwrite/discard trace and independently regenerate
 * the expected bytes for every logical address after each phase. */
#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef uint8_t u8;
typedef uint32_t u32;
enum { PAGE = 4096, PAGES = 65536 };
#include "capacity_pattern.h"

struct description { u32 seed; char kind; };
static struct description oracle[PAGES];

static void generate(struct description d, u8 *out)
{
    if (!d.kind) {
        memset(out, 0, PAGE);
    } else if (d.kind == 'C') {
        /* These distinct pages deliberately collide on all three samples. */
        expected(d.seed, 'U', out);
        memset(out, 0, 8);
        memset(out + PAGE / 2, 0, 8);
        memset(out + PAGE - 8, 0, 8);
    } else {
        expected(d.seed, d.kind, out);
    }
}

static double seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void)
{
    struct store *store = store_new(PAGES);
    if (!store) return 1;
    u8 want[PAGE], got[PAGE];
    uint64_t stats[5], unique, hits, fast, residuals;
    store_stats(store, stats);
    uint64_t empty = stats[4];
    for (unsigned phase = 0; phase < 6; ++phase) {
        double start = seconds();
        if (phase == 0) {
            for (u32 i = 0; i < PAGES; ++i) {
                oracle[i] = (struct description){i, 'W'};
                generate(oracle[i], want);
                if (store_put(store, i, want)) return 2;
            }
        } else if (phase == 3 || phase == 5) {
            u32 first = phase == 3 ? 37 : 0;
            u32 count = phase == 3 ? PAGES * 3 / 4 : PAGES;
            if (store_discard(store, first, count)) return 2;
            memset(oracle + first, 0, count * sizeof(*oracle));
        } else {
            static const char kinds[] = {'W', 'U', 'D', 'F', 'S', 'C', 0, 0};
            for (u32 j = 0; j < PAGES / 2; ++j) {
                u32 i = (j * 40503 + phase * 997) % PAGES;
                u32 seed = mix(i + phase * PAGES);
                oracle[i] = (struct description){seed, kinds[seed % sizeof(kinds)]};
                if (oracle[i].kind) {
                    generate(oracle[i], want);
                    if (store_put(store, i, want)) return 2;
                } else if (store_discard(store, i, 1)) {
                    return 2;
                }
            }
        }
        double written = seconds();
        for (u32 j = 0; j < PAGES; ++j) {
            u32 i = (j * 23131) % PAGES;
            generate(oracle[i], want);
            if (store_get(store, i, got) || memcmp(want, got, PAGE)) return 3;
        }
        double verified = seconds();
        store_stats(store, stats);
        store_literal_stats(store, &unique, &hits, &fast, &residuals);
        if (phase == 5 && (unique || stats[4] != empty)) return 4;
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage)) return 5;
        printf("{\"phase\":%u,\"verified_pages\":%u,\"store_bytes\":%lu,"
               "\"unique_literals\":%lu,\"peak_rss_kib\":%ld,\"update_seconds\":%.6f,"
               "\"verify_seconds\":%.6f}\n", phase, PAGES, stats[4], unique,
               usage.ru_maxrss, written-start, verified-written);
    }
    store_free(store);
    return 0;
}
