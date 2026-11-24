/* Linux PID 1 for paired ordinary-QEMU and vRAM first-touch measurements. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, PAGES = 16384, MAX_CPUS = 8 };
typedef uint32_t u32;
typedef uint8_t u8;
#include "capacity_pattern.h"

static unsigned cpus;
static u8 *memory;
static uint64_t latency[PAGES];
static pthread_barrier_t ready, start, written;

static void fail(const char *why)
{
    printf("PARALLEL_FAILURE %s\n", why);
    reboot(RB_POWER_OFF);
    _exit(1);
}

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *worker(void *arg)
{
    unsigned id = *(unsigned *)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(id, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) fail("affinity");
    unsigned first = PAGES * id / cpus, end = PAGES * (id + 1) / cpus;
    u8 page[PAGE];
    expected(0, 'U', page);
    pthread_barrier_wait(&ready);
    pthread_barrier_wait(&start);
    for (unsigned i = first; i < end; ++i) {
        uint32_t at = i * 4051 % PAGES;
        memcpy(page + 128, &at, sizeof(at));
        uint64_t t = now();
        memcpy(memory + (size_t)at * PAGE, page, PAGE);
        latency[i] = now() - t;
    }
    pthread_barrier_wait(&written);
    for (unsigned i = first; i < end; ++i) {
        uint32_t at = i * 4051 % PAGES;
        memcpy(page + 128, &at, sizeof(at));
        if (memcmp(memory + (size_t)at * PAGE, page, PAGE)) fail("readback");
    }
    return NULL;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mount("sysfs", "/sys", "sysfs", 0, NULL)) fail("sysfs");
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (!cpus || cpus > MAX_CPUS) fail("CPU count");
    memory = mmap(NULL, (size_t)PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED || madvise(memory, (size_t)PAGES * PAGE, MADV_NOHUGEPAGE)) fail("mapping");
    if (pthread_barrier_init(&ready, NULL, cpus + 1) || pthread_barrier_init(&start, NULL, cpus + 1)
        || pthread_barrier_init(&written, NULL, cpus + 1)) fail("barrier");
    pthread_t threads[MAX_CPUS];
    unsigned ids[MAX_CPUS];
    for (unsigned i = 0; i < cpus; ++i) {
        ids[i] = i;
        if (pthread_create(&threads[i], NULL, worker, &ids[i])) fail("thread");
    }
    pthread_barrier_wait(&ready);
    uint64_t begin = now();
    pthread_barrier_wait(&start);
    pthread_barrier_wait(&written);
    uint64_t elapsed = now() - begin;
    for (unsigned i = 0; i < cpus; ++i)
        if (pthread_join(threads[i], NULL)) fail("join");
    qsort(latency, PAGES, sizeof(*latency), compare);
    printf("PARALLEL_RESULT {\"vcpus\":%u,\"verified_pages\":%u,\"write_ns\":%" PRIu64
           ",\"page_write_p99_ns\":%" PRIu64 "}\n", cpus, PAGES, elapsed,
           latency[(PAGES * 99 + 99) / 100 - 1]);
    reboot(RB_POWER_OFF);
    return 1;
}
