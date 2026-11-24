// SPDX-License-Identifier: MIT OR Apache-2.0
#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "pager.h"
#include "projection_ioctl.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Both inodes address disjoint ranges of the same Verdant dictionary. */
enum { PAGE = 4096 };
struct extent { size_t first, bytes; struct timespec times[2]; };
static struct extent boot, dimm;
static struct store *memory;
static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t reads, writes, read_service_ns, write_service_ns;
static bool profile_io;

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static struct extent *extent_for(const char *path)
{
    if (!strcmp(path, "/boot")) return &boot;
    if (dimm.bytes && !strcmp(path, "/dimm")) return &dimm;
    return NULL;
}

static int attributes(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    if (!strcmp(path, "/")) {
        st->st_mode = S_IFDIR | 0700;
        st->st_nlink = 2;
        return 0;
    }
    struct extent *extent = extent_for(path);
    if (!extent) return -ENOENT;
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_size = extent->bytes;
    st->st_blksize = PAGE;
    pthread_mutex_lock(&time_lock);
    st->st_atim = extent->times[0];
    st->st_mtim = st->st_ctim = extent->times[1];
    pthread_mutex_unlock(&time_lock);
    return 0;
}

static int list(const char *path, void *buf, fuse_fill_dir_t fill, off_t off,
                struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)fi; (void)flags;
    if (strcmp(path, "/")) return -ENOENT;
    const char *names[] = {".", "..", "boot", "dimm"};
    for (off_t i = off; i >= 0 && i < (dimm.bytes ? 4 : 3); ++i)
        if (fill(buf, names[i], NULL, i + 1, 0)) break;
    return 0;
}

static int page_io(const char *path, char *out, const char *in, size_t len, off_t off)
{
    struct extent *extent = extent_for(path);
    if (!extent) return -ENOENT;
    if (off < 0 || len > INT_MAX) return -EINVAL;
    size_t pos = (size_t)off;
    if (pos >= extent->bytes) return in && len ? -ENOSPC : 0;
    if (len > extent->bytes - pos) {
        if (in) return -ENOSPC;
        len = extent->bytes - pos;
    }
    size_t done = 0;
    while (done < len) {
        uint8_t scratch[PAGE];
        size_t page = extent->first + pos / PAGE, start = pos % PAGE;
        size_t n = PAGE - start < len - done ? PAGE - start : len - done;
        bool whole = n == PAGE;
        if (whole) {
            size_t pages = (len - done) / PAGE;
            int rc = in ? store_write_pages(memory, page, pages, (const uint8_t *)in + done)
                        : store_read_pages(memory, page, pages, (uint8_t *)out + done);
            if (rc) return -EIO;
            n = pages * PAGE;
        } else if (in) {
            if (store_patch(memory, page, start, (const uint8_t *)in + done, n)) return -EIO;
        } else {
            if (store_get(memory, page, scratch)) return -EIO;
            memcpy(out + done, scratch + start, n);
        }
        pos += n;
        done += n;
    }
    size_t pages = len ? ((size_t)off % PAGE + len + PAGE - 1) / PAGE : 0;
    atomic_fetch_add_explicit(in ? &writes : &reads, pages, memory_order_relaxed);
    return (int)done;
}
static int ram_read(const char *p, char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    uint64_t start = profile_io ? monotonic_ns() : 0;
    int rc = page_io(p, buf, NULL, len, off);
    if (profile_io) atomic_fetch_add_explicit(&read_service_ns, monotonic_ns() - start, memory_order_relaxed);
    return rc;
}
static int ram_write(const char *p, const char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    uint64_t start = profile_io ? monotonic_ns() : 0;
    int rc = page_io(p, NULL, buf, len, off);
    if (profile_io) atomic_fetch_add_explicit(&write_service_ns, monotonic_ns() - start, memory_order_relaxed);
    return rc;
}
/* Linux flushes inode timestamps before issuing FSYNC for dirty shared mappings. */
static int ram_times(const char *path, const struct timespec times[2], struct fuse_file_info *fi)
{
    (void)fi;
    struct extent *extent = extent_for(path);
    if (!extent) return -ENOENT;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return -errno;
    pthread_mutex_lock(&time_lock);
    for (size_t i = 0; i < 2; ++i)
        if (times[i].tv_nsec != UTIME_OMIT)
            extent->times[i] = times[i].tv_nsec == UTIME_NOW ? now : times[i];
    pthread_mutex_unlock(&time_lock);
    return 0;
}

static int ram_sync(const char *p, int datasync, struct fuse_file_info *fi)
{
    (void)p; (void)datasync; (void)fi;
    /* All preceding FUSE writes have committed to the volatile page store. */
    return 0;
}
static int ram_ioctl(const char *path, int cmd, void *arg, struct fuse_file_info *fi,
                     unsigned flags, void *data)
{
    (void)arg; (void)fi; (void)flags;
    if ((unsigned)cmd != VRAM_PROJECT_IOCTL) return -ENOTTY;
    if (extent_for(path) != &dimm || !data) return -EINVAL;
    struct vram_projection q;
    memcpy(&q.request, data, sizeof(q.request));
    if (q.request.operation != VRAM_PROJECT || q.request.reserved ||
        !vram_range(q.request.offset, PAGE, dimm.bytes)) return -EINVAL;
    int route = store_project(memory, dimm.first + q.request.offset / PAGE,
                              q.request.weights, q.columns);
    if (route < 0) return -EIO;
    memcpy(data, &q, sizeof(q));
    return route;
}

static void *initialize(struct fuse_conn_info *conn, struct fuse_config *conf)
{
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    conf->kernel_cache = 1;
    return NULL;
}
static bool number(const char *s, uint64_t *out)
{
    if (*s < '0' || *s > '9') return false;
    char *end;
    errno = 0;
    *out = strtoull(s, &end, 10);
    return !errno && !*end;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--serve-projection"))
        return serve_projection(argv[2]);
    uint64_t boot_mib, dimm_mib, budget;
    if (argc == 5 && !strcmp(argv[1], "--ceiling")) {
        if (!number(argv[2], &boot_mib) || !number(argv[3], &dimm_mib) ||
            !number(argv[4], &budget)) return 2;
        printf("%" PRIu64 "\n", vram_capacity_ceiling(boot_mib, dimm_mib, budget));
        return 0;
    }
    if (argc != 5 || !number(argv[1], &boot_mib) || !boot_mib ||
        !number(argv[2], &dimm_mib) || sizeof(size_t) < 8 || sysconf(_SC_PAGESIZE) != PAGE)
        return 2;
    /* Both FUSE extents and the extra replacement slot have signed file offsets. */
    uint64_t max_mib = vram_capacity_ceiling(UINT64_MAX, UINT64_MAX, INT64_MAX);
    if (boot_mib > max_mib || dimm_mib > max_mib - boot_mib) return 2;
    boot.bytes = boot_mib << 20;
    dimm.bytes = dimm_mib << 20;
    dimm.first = boot.bytes / PAGE;
    char *end;
    long fd = strtol(argv[3], &end, 10);
    if (end == argv[3] || *end || fd < 0 || fd > INT_MAX) return 2;
    memory = store_new_file((boot.bytes + dimm.bytes) / PAGE, (int)fd);
    close((int)fd);
    if (!memory) return 1;
    const struct fuse_operations ops = { .getattr = attributes, .readdir = list,
        .ioctl = ram_ioctl, .read = ram_read, .write = ram_write, .fsync = ram_sync, .utimens = ram_times, .init = initialize };
    const char *profile = getenv("VRAM_PROFILE_IO");
    profile_io = profile && !strcmp(profile, "1");
    char *args[] = {argv[0], "-f", "-o", "default_permissions", argv[4]};
    int rc = fuse_main(5, args, &ops, NULL);
    uint64_t stats[5], disk_reads, disk_writes;
    store_file_stats(memory, &disk_reads, &disk_writes);
    store_stats(memory, stats);
    fprintf(stderr, "VRAM_FILE {\"read_pages\":%" PRIu64 ",\"write_pages\":%" PRIu64
            ",\"file_base_reads\":%" PRIu64 ",\"file_base_writes\":%" PRIu64
            ",\"store_bytes\":%" PRIu64 "}\n", reads, writes, disk_reads, disk_writes, stats[4]);
    if (profile_io) {
        uint64_t ns[4];
        uint64_t fits[2];
        store_fit_counts(memory, fits);
        uint64_t fit_profile[7];
        store_fit_profile(memory, fit_profile);
        fprintf(stderr, "VRAM_FITS {\"prepared\":%" PRIu64 ",\"used\":%" PRIu64
                ",\"encoding_only\":%" PRIu64 ",\"fit_ns\":%" PRIu64 ",\"commit_overlap_ns\":%" PRIu64
                ",\"snapshot_ns\":%" PRIu64 ",\"validation_ns\":%" PRIu64 ",\"credit_replays\":%" PRIu64
                ",\"reacquire_ns\":%" PRIu64 ",\"reacquire_commit_ns\":%" PRIu64 "}\n",
                fits[0], fits[1], fit_profile[0], fit_profile[1], fit_profile[2], fit_profile[3], fit_profile[4], fit_profile[5], fit_profile[6],
                store_reacquire_commit_ns(memory));
        store_io_times(memory, ns);
        fprintf(stderr, "VRAM_TIMES {\"read_callback_ns\":%" PRIu64 ",\"write_callback_ns\":%" PRIu64
                ",\"read_lock_ns\":%" PRIu64 ",\"write_lock_ns\":%" PRIu64
                ",\"pread_ns\":%" PRIu64 ",\"pwrite_ns\":%" PRIu64 "}\n",
                (uint64_t)read_service_ns, (uint64_t)write_service_ns, ns[0], ns[1], ns[2], ns[3]);
    }
    store_free(memory);
    return rc;
}
