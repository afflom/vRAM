/* Time file-backed full-page writes through the public ABI, then check every byte. */
#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, THREADS = 8, PAGES = 4096, BATCH = 4 };
typedef uint32_t u32;
typedef uint8_t u8;
#include "capacity_pattern.h"

static u8 payload[PAGES][PAGE];
static u8 restored[PAGES][PAGE];
static uint64_t latency[PAGES / BATCH];
static struct store *store;
static pthread_barrier_t start, finish;

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *worker(void *arg)
{
    size_t lane = *(size_t *)arg;
    pthread_barrier_wait(&start);
    for (size_t first = lane * BATCH; first < PAGES; first += THREADS * BATCH)
        if (store_write_pages(store, first, BATCH, payload[first])) abort();
    pthread_barrier_wait(&finish);
    return NULL;
}

static int compare_ns(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc != 2 || !argv[1][0] || strspn(argv[1], "UDPFA") != strlen(argv[1])) {
        fprintf(stderr, "usage: %s PROFILES (U independent, D sparse edits, P packed, F tiled, A adjacent residuals)\n", argv[0]);
        return 2;
    }
    if (pthread_barrier_init(&start, NULL, THREADS + 1) ||
        pthread_barrier_init(&finish, NULL, THREADS + 1)) return 1;
    for (const char *profile = argv[1]; *profile; ++profile) {
        for (size_t i = 0; i < PAGES; ++i) {
            if (*profile == 'A') {
                expected(i / BATCH, 'U', payload[i]);
                payload[i][128] ^= i % BATCH;
            } else {
                expected(i, *profile == 'P' ? 'U' : *profile, payload[i]);
            }
            if (*profile == 'P')
                for (size_t k = 0; k < PAGE; ++k) payload[i][k] &= 15;
        }
        FILE *file = tmpfile();
        if (!file || posix_fallocate(fileno(file), 0, (PAGES + 1) * PAGE)) return 1;
        store = store_new_file(PAGES, fileno(file));
        if (!store) return 1;
        pthread_t threads[THREADS];
        size_t lanes[THREADS];
        for (size_t i = 0; i < THREADS; ++i) {
            lanes[i] = i;
            if (pthread_create(&threads[i], NULL, worker, &lanes[i])) abort();
        }
        uint64_t begin = now();
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&finish);
        uint64_t elapsed = now() - begin;
        for (size_t i = 0; i < THREADS; ++i)
            if (pthread_join(threads[i], NULL)) abort();

        memset(restored, 0xa5, sizeof(restored));
        uint64_t before, after, writes;
        store_file_stats(store, &before, &writes);
        uint64_t read_start = now();
        for (size_t i = 0; i < PAGES; i += BATCH) {
            uint64_t t = now();
            if (store_read_pages(store, i, BATCH, restored[i])) abort();
            latency[i / BATCH] = now() - t;
        }
        uint64_t read_ns = now() - read_start;
        store_file_stats(store, &after, &writes);
        if (memcmp(payload, restored, sizeof(payload))) {
            fprintf(stderr, "range readback failure in profile %c\n", *profile);
            return 1;
        }
        qsort(latency, PAGES / BATCH, sizeof(*latency), compare_ns);
        printf("{\"profile\":\"%c\",\"write_ns\":%" PRIu64 ",\"read_ns\":%" PRIu64
               ",\"range_read_p99_ns\":%" PRIu64 ",\"write_phase_base_reads\":%" PRIu64
               ",\"read_phase_base_reads\":%" PRIu64 ",\"verified_pages\":%d}\n",
               *profile, elapsed, read_ns, latency[((PAGES / BATCH) * 99 + 99) / 100 - 1],
               before, after - before, PAGES);
        store_free(store);
        fclose(file);
    }
    pthread_barrier_destroy(&finish);
    pthread_barrier_destroy(&start);
    return 0;
}
