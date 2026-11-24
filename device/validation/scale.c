#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef uint8_t u8;
typedef uint32_t u32;
enum { PAGE = 4096 };
#include "capacity_pattern.h"

/* The comparison harness also links releases predating literal statistics. */
#pragma weak store_literal_stats

static long peak(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) abort();
    return ru.ru_maxrss;
}
static double elapsed(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    long mib = atol(argv[1]), budget = atol(argv[2]), nodes = atol(argv[4]);
    char profile = argv[3][0];
    if (mib < 1 || mib > 1048576 || budget < 1 || budget > 1048576 ||
        nodes < 0 || nodes > 49 || strlen(argv[3]) != 1 || !strchr("SFUMRDW", profile)) return 2;
    size_t target = (size_t)mib * 256, count = 0, verified = 0;
    uint8_t original[PAGE], decoded[PAGE];
    struct store *s = store_new_config(target, nodes, true);
    if (!s) return 3;
    double start = elapsed();
    while (count < target) {
        if (!(count % 64) && peak() > budget * 1024) break;
        expected(count, profile, original);
        if (store_put(s, count, original)) return 4;
        ++count;
    }
    double written = elapsed();
    for (; verified < count; ++verified) {
        expected(verified, profile, original);
        if (store_get(s, verified, decoded) || memcmp(original, decoded, PAGE)) break;
    }
    uint64_t stats[5], refits, skips;
    store_stats(s, stats);
    store_feedback_stats(s, &refits, &skips);
    printf("{\"requested_mib\":%ld,\"profile\":\"%c\",\"search_nodes\":%ld,"
           "\"stored_pages\":%zu,\"verified_pages\":%zu,\"peak_rss_kib\":%ld,"
           "\"store_bytes\":%lu,\"raw_pages\":%lu,\"refits\":%lu,\"skips\":%lu,"
           "\"write_seconds\":%.6f,\"read_seconds\":%.6f",
           mib, profile, nodes, count, verified, peak(), stats[4], stats[2], refits, skips,
           written - start, elapsed() - written);
    if (store_literal_stats) {
        uint64_t bases, hits, fast, residuals;
        store_literal_stats(s, &bases, &hits, &fast, &residuals);
        printf(",\"unique_literals\":%lu,\"literal_hits\":%lu", bases, hits);
    }
    puts("}");
    store_free(s);
    return 0;
}
