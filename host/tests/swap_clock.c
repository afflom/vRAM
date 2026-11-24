// Link only into the diagnostic NBD server. The caller reads the shared record
// after the NBD reply, before issuing the next request at queue depth one.
#define _GNU_SOURCE
#include "pager.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct sample { uint64_t sequence, page, write, enter, leave; };
static struct sample *latest;

__attribute__((constructor)) static void open_clock(void)
{
    int fd = open("/store-clock", O_RDWR);
    if (fd < 0) abort();
    latest = mmap(NULL, sizeof(*latest), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (latest == MAP_FAILED) abort();
    close(fd);
}

static uint64_t now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int __real_store_get(struct store *, size_t, uint8_t *);
int __real_store_put(struct store *, size_t, const uint8_t *);

int __wrap_store_get(struct store *s, size_t page, uint8_t *bytes)
{
    uint64_t enter = now();
    int rc = __real_store_get(s, page, bytes);
    uint64_t leave = now();
    *latest = (struct sample){latest->sequence + 1, page, 0, enter, leave};
    return rc;
}

int __wrap_store_put(struct store *s, size_t page, const uint8_t *bytes)
{
    uint64_t enter = now();
    int rc = __real_store_put(s, page, bytes);
    uint64_t leave = now();
    *latest = (struct sample){latest->sequence + 1, page, 1, enter, leave};
    return rc;
}
