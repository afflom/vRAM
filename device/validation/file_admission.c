/* Exercise admission and write errors through the same ABI as the Linux RAM device. */
#define _GNU_SOURCE
#include "pager.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE = 4096, PAGES = 32769 };
typedef uint8_t u8;
typedef uint32_t u32;
#include "capacity_pattern.h"

static int verify(struct store *s, size_t at, const u8 *expected_bytes)
{
    u8 restored[PAGE];
    return !store_get(s, at, restored) && !memcmp(restored, expected_bytes, PAGE);
}

static int collisions(struct store *s)
{
    u8 a[PAGE], b[PAGE], edited[PAGE];
    expected(1, 'U', a);
    memcpy(b, a, PAGE);
    for (int i = 0; i < 80; ++i) b[100 + i] ^= 37;
    memcpy(edited, a, PAGE);
    memcpy(edited + 100, b + 100, 50);
    if (store_put(s, 0, a) || store_put(s, 1, b)) return 0;
    uint64_t before, after, writes, hits, bases, skipped, residuals;
    store_file_stats(s, &before, &writes);
    if (store_put(s, 2, edited) || store_put(s, 3, b)) return 0;
    store_file_stats(s, &after, &writes);
    store_literal_stats(s, &bases, &hits, &skipped, &residuals);
    if (after - before != 4 || writes != 2 || hits != 1 || residuals != 1 || bases != 2) {
        fprintf(stderr, "collision admission used %lu base reads, expected 4\n", after - before);
        return 0;
    }
    return !store_discard(s, 0, 2) && verify(s, 2, edited) && verify(s, 3, b);
}

static int edit_limit(struct store *s)
{
    u8 a[PAGE], near[PAGE], far[PAGE];
    expected(23, 'U', a);
    memcpy(near, a, PAGE);
    for (int i = 0; i < 64; ++i) near[8 + i * 8] ^= 1;
    memcpy(far, near, PAGE);
    far[8 + 64 * 8] ^= 1;
    uint64_t bases, hits, skipped, before, after;
    store_literal_stats(s, &bases, &hits, &skipped, &before);
    if (store_discard(s, 0, PAGES) || store_put(s, 0, a) ||
        store_put(s, 1, near) || store_put(s, 2, far)) return 0;
    store_literal_stats(s, &bases, &hits, &skipped, &after);
    return bases == 2 && after == before + 1 && verify(s, 1, near) && verify(s, 2, far);
}

static int failed_writes(struct store *s, int fd)
{
    u8 old[PAGE], other[PAGE], zero[PAGE] = {0};
    expected(17, 'U', old);
    expected(19, 'U', other);
    if (store_discard(s, 0, PAGES) || store_put(s, 0, old)) return 0;
    uint64_t before[5], after[5];
    store_stats(s, before);
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE)) return 0;
    if (!store_put(s, 0, other) || !verify(s, 0, old)) return 0;
    // Previously empty lanes, groups, and leaves, including a partial final leaf.
    size_t addresses[] = {1, 64, 16384, PAGES - 1};
    for (int repeat = 0; repeat < 2; ++repeat)
        for (size_t i = 0; i < sizeof(addresses) / sizeof(*addresses); ++i)
            if (!store_put(s, addresses[i], other) || !verify(s, addresses[i], zero)) return 0;
    store_stats(s, after);
    if (after[4] != before[4]) {
        fprintf(stderr, "failed writes changed store charge from %lu to %lu bytes\n", before[4], after[4]);
        return 0;
    }
    return verify(s, 0, old);
}

int main(void)
{
    int fd = memfd_create("vram-admission", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)(PAGES + 1) * PAGE)) return 1;
    struct store *s = store_new_file(PAGES, fd);
    if (!s) return 1;
    int fit = collisions(s);
    int boundary = edit_limit(s);
    int rollback = failed_writes(s, fd);
    store_free(s);
    close(fd);
    return !(fit && boundary && rollback);
}
