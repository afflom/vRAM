/* Same probe for the previous and current library. All extents are logical. */
#define _GNU_SOURCE
#include "pager.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <unistd.h>

static void pattern(uint8_t *page, unsigned seed)
{
    for (int i = 0; i < 4096; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        page[i] = (uint8_t)seed;
    }
}

int main(int argc, char **argv)
{
    unsigned long tib = argc >= 2 ? strtoul(argv[1], NULL, 10) : 1024;
    if (!tib || tib > 1024 || sizeof(size_t) < 8) return 2;
    struct rlimit budget = {64UL << 20, 64UL << 20};
    if (setrlimit(RLIMIT_AS, &budget)) return 2;
    size_t pages = (size_t)tib << 28;
    size_t at[] = {0, 63, 64, 16383, 16384, pages / 2, pages - 2, pages - 1};
    int file = argc == 3 && !strcmp(argv[2], "file");
    struct store *s;
    if (file) {
        /* A sparse file exercises the ledger and I/O at scale in this probe.
         * The installed launcher separately requires full preallocation. */
        int fd = memfd_create("sparse-extent-test", 0);
        if (fd < 0 || ftruncate(fd, (off_t)(pages + 1) * 4096)) return 2;
        s = store_new_file(pages, fd);
        close(fd);
    } else {
        s = store_new(pages);
    }
    if (!s) { fprintf(stderr, "directory refused %lu TiB\n", tib); return 1; }
    uint64_t before[5], active[5], after[5];
    uint8_t page[4096], got[4096];
    store_stats(s, before);
    memset(page, 0, sizeof(page));
    if (store_get(s, pages - 3, got) || memcmp(page, got, sizeof(page)) ||
        !store_get(s, pages, got) || store_discard(s, pages, 0) ||
        !store_discard(s, pages - 1, 2)) return 1;
    for (size_t i = 0; i < sizeof(at) / sizeof(*at); ++i) {
        pattern(page, (unsigned)i + 1);
        if (store_put(s, at[i], page)) return 1;
    }
    store_stats(s, active);
    for (int phase = 0; phase < 3; ++phase) {
        for (size_t i = 0; i < sizeof(at) / sizeof(*at); ++i) {
            int live = phase == 0 || (phase == 1 && (at[i] < 63 || at[i] == pages - 1));
            pattern(page, live ? (unsigned)i + 1 : 0);
            if (store_get(s, at[i], got) || memcmp(page, got, sizeof(page))) return 1;
        }
        if (phase == 0 && store_discard(s, 63, pages - 64)) return 1;
        if (phase == 1 && store_discard(s, 0, pages)) return 1;
    }
    store_stats(s, after);
    store_free(s);
    if (memcmp(before, after, sizeof(after))) return 1;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) return 2;
    printf("{\"logical_tib\":%lu,\"sparse_file\":%s,\"empty_store_bytes\":%" PRIu64
           ",\"active_store_bytes\":%" PRIu64 ",\"peak_rss_kib\":%ld}\n",
           tib, file ? "true" : "false", before[4], active[4], usage.ru_maxrss);
    return 0;
}
