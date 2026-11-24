/* Exercise admission and write errors through the same ABI as the Linux RAM device. */
#define _GNU_SOURCE
#include "pager.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
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

static int shared_range(struct store *s)
{
    /* The run starts on a residual and crosses a group boundary. Two distinct
     * dense bases have identical fingerprint samples. Page 68 is a hole. */
    enum { FIRST = 62, COUNT = 8 };
    u8 want[COUNT][PAGE], out[COUNT][PAGE];
    expected(0, 'C', want[1]);
    expected(1, 'C', want[4]);
    unsigned bases[] = {1, 1, 1, 1, 4, 4, 0, 1};
    if (store_discard(s, 0, PAGES) || store_put(s, 0, want[1]) || store_put(s, 1, want[4]))
        return 0;
    for (unsigned i = 0; i < COUNT; ++i) {
        if (i == 6) {
            memset(want[i], 0, PAGE);
            continue;
        }
        if (bases[i] != i) memcpy(want[i], want[bases[i]], PAGE);
        if (i == 0 || i == 2 || i == 3 || i == 5) want[i][125 + i] ^= 73;
        if (store_put(s, FIRST + i, want[i])) return 0;
    }
    uint64_t before, after, writes;
    store_file_stats(s, &before, &writes);
    if (store_read_pages(s, FIRST, COUNT, out[0]) || memcmp(out, want, sizeof(out))) return 0;
    store_file_stats(s, &after, &writes);
    if (after - before != 4) {
        fprintf(stderr, "range restore used %lu backing reads, expected 4\n", after - before);
        return 0;
    }
    return 1;
}

static int shared_admissions(struct store *s)
{
    u8 base[PAGE], batch[5][PAGE];
    expected(23, 'U', base);
    if (store_discard(s, 0, PAGES) || store_put(s, 0, base)) return 0;
    for (unsigned i = 0; i < 5; ++i) {
        memcpy(batch[i], base, PAGE);
        if (i != 2) batch[i][347 + i] ^= 31;
    }
    uint64_t before, after, writes, previous_writes;
    store_file_stats(s, &before, &previous_writes);
    if (store_write_pages(s, 62, 5, batch[0])) return 0;
    store_file_stats(s, &after, &writes);
    if (after - before != 1 || writes != previous_writes) {
        fprintf(stderr, "range admission used %lu backing reads, expected 1\n", after - before);
        return 0;
    }
    for (unsigned i = 0; i < 5; ++i)
        if (!verify(s, 62 + i, batch[i])) return 0;
    return 1;
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

static int converging_overwrites(struct store *s)
{
    enum { N = 4096 };
    u8 page[PAGE];
    if (store_discard(s, 0, PAGES)) return 0;
    for (int i = 0; i < N; ++i) {
        expected(i, 'U', page);
        if (store_put(s, i, page)) return 0;
    }
    expected(0, 'U', page);
    for (int i = 1; i < N; ++i)
        if (store_put(s, i, page)) return 0;
    uint64_t before[5], after[5];
    store_stats(s, before);
    if (store_discard(s, N, 1)) return 0;
    store_stats(s, after);
    if (before[4] != after[4]) return 0;
    for (int i = 0; i < N; ++i)
        if (!verify(s, i, page)) return 0;
    return 1;
}

static int incumbent_edits(struct store *s)
{
    u8 base[PAGE], page[PAGE], independent[PAGE];
    expected(29, 'U', base);
    expected(30, 'U', independent);
    memcpy(page, base, PAGE);
    if (store_discard(s, 0, PAGES) || store_put(s, 0, base) || store_put(s, 1, base)) return 0;
    uint64_t initial_reads, initial_writes, reads, writes;
    store_file_stats(s, &initial_reads, &initial_writes);
    /* An indexed match and two successive fingerprint misses must each read the
     * original immutable base exactly once, including when the incumbent is a residual. */
    size_t offsets[] = {100, 0, PAGE - 1};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        page[offsets[i]] ^= 1;
        if (store_put(s, 1, page)) return 0;
    }
    store_file_stats(s, &reads, &writes);
    if (reads != initial_reads + 3 || writes != initial_writes ||
        !verify(s, 0, base) || !verify(s, 1, page)) return 0;
    if (store_discard(s, 0, 1) || !verify(s, 1, page)) return 0;
    /* A sole-owned base with a different fingerprint needs no nomination read. */
    store_file_stats(s, &initial_reads, &initial_writes);
    if (store_put(s, 1, independent)) return 0;
    store_file_stats(s, &reads, &writes);
    if (reads != initial_reads || writes != initial_writes + 1 || !verify(s, 1, independent)) return 0;
    /* With the same fingerprint, an exact hit is allowed but retaining an
     * exclusive base solely for a residual would cost more than a new literal. */
    independent[100] ^= 1;
    if (store_put(s, 1, independent)) return 0;
    store_file_stats(s, &reads, &writes);
    return writes == initial_writes + 2 && verify(s, 1, independent);
}

/* Run on a fresh file so the spare slot is known to start at byte PAGE. */
static int partial_write(struct store *s, int fd)
{
    u8 old[PAGE], partial[PAGE], replacement[PAGE], disk[PAGE];
    expected(1, 'U', old);
    expected(2, 'U', partial);
    expected(3, 'U', replacement);
    if (store_put(s, 0, old)) return 0;
    struct rlimit original;
    if (getrlimit(RLIMIT_FSIZE, &original)) return 0;
    struct rlimit limit = original;
    limit.rlim_cur = PAGE + 1024;
    struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGXFSZ, &ignore, &previous)) return 0;
    int limited = !setrlimit(RLIMIT_FSIZE, &limit);
    int refused = limited && store_put(s, 0, partial) != 0;
    if (setrlimit(RLIMIT_FSIZE, &original) || sigaction(SIGXFSZ, &previous, NULL)) return 0;
    u8 zero[PAGE] = {0};
    if (!refused || pread(fd, disk, PAGE, PAGE) != PAGE ||
        memcmp(disk, partial, 1024) || memcmp(disk + 1024, zero, PAGE - 1024) ||
        !verify(s, 0, old) || !verify(s, 1, zero)) return 0;
    if (store_put(s, 1, replacement) || !verify(s, 0, old) || !verify(s, 1, replacement) ||
        pread(fd, disk, PAGE, PAGE) != PAGE || memcmp(disk, replacement, PAGE)) return 0;
    return !store_discard(s, 0, PAGES);
}

static int failed_writes(struct store *s, int fd)
{
    u8 old[PAGE], other[PAGE], zero[PAGE] = {0};
    expected(17, 'U', old);
    expected(19, 'U', other);
    if (store_discard(s, 0, PAGES) || store_put(s, 0, old)) return 0;
    /* Fill four whole slot words. A failed write would start a fifth and
     * grow the ledger if its reservation were committed before successful I/O. */
    u8 live[PAGE];
    for (size_t i = 1; i < 256; ++i) {
        expected((u32)i + 1000, 'U', live);
        if (store_put(s, 1024 + i, live)) return 0;
    }
    uint64_t bases, hits, skipped, residuals;
    store_literal_stats(s, &bases, &hits, &skipped, &residuals);
    if (bases != 256) return 0;
    uint64_t before[5], after[5];
    store_stats(s, before);
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE)) return 0;
    if (!store_put(s, 0, other) || !verify(s, 0, old)) return 0;
    if (!store_patch(s, 0, 1, other + 1, PAGE - 2) || !verify(s, 0, old)) return 0;
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
    // The first page commits as a fill. The sealed file rejects the next dense page.
    u8 batch[3 * PAGE];
    memset(batch, 37, PAGE);
    memcpy(batch + PAGE, other, PAGE);
    memset(batch + 2 * PAGE, 57, PAGE);
    if (store_put(s, 64, old) || !store_write_pages(s, 63, 3, batch) ||
        !verify(s, 63, batch) || !verify(s, 64, old) || !verify(s, 65, zero)) return 0;
    return verify(s, 0, old);
}

int main(void)
{
    int fd = memfd_create("vram-admission", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)(PAGES + 1) * PAGE)) return 1;
    struct store *s = store_new_file(PAGES, fd);
    if (!s) return 1;
    int torn = partial_write(s, fd);
    if (!torn) fprintf(stderr, "partial write rollback or spare-slot reuse failed\n");
    store_free(s);
    s = store_new_file(PAGES, fd);
    if (!s) return 1;
    int fit = collisions(s);
    int ranges = shared_range(s);
    int admissions = shared_admissions(s);
    int boundary = edit_limit(s);
    int convergence = converging_overwrites(s);
    int edits = incumbent_edits(s);
    if (!convergence) fprintf(stderr, "dictionary shrink was deferred after convergence\n");
    if (!edits) fprintf(stderr, "incumbent ownership or admission I/O differs\n");
    int rollback = failed_writes(s, fd);
    store_free(s);
    close(fd);
    return !(torn && fit && ranges && admissions && boundary && convergence && edits && rollback);
}
