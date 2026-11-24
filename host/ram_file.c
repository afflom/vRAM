// SPDX-License-Identifier: MIT OR Apache-2.0
#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "pager.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Each inode is an immutable extent of one VM-wide Verdant page dictionary.
 * The single FUSE dispatch thread serializes partial-page updates. */
enum { PAGE = 4096 };
struct extent { size_t first, bytes; struct timespec times[2]; };
static struct extent boot, dimm;
static struct store *memory;
static uint64_t reads, writes;

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
    st->st_atim = extent->times[0];
    st->st_mtim = st->st_ctim = extent->times[1];
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
        if (in) {
            const uint8_t *payload = (const uint8_t *)in + done;
            if (!whole) {
                if (store_get(memory, page, scratch)) return -EIO;
                memcpy(scratch + start, payload, n);
                payload = scratch;
            }
            if (store_put(memory, page, payload)) return -EIO;
            writes++;
        } else {
            uint8_t *dest = (uint8_t *)out + done;
            if (store_get(memory, page, whole ? dest : scratch)) return -EIO;
            if (!whole) memcpy(dest, scratch + start, n);
            reads++;
        }
        pos += n;
        done += n;
    }
    return (int)done;
}
static int ram_read(const char *p, char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    return page_io(p, buf, NULL, len, off);
}
static int ram_write(const char *p, const char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    return page_io(p, NULL, buf, len, off);
}
/* Linux flushes inode timestamps before issuing FSYNC for dirty shared mappings. */
static int ram_times(const char *path, const struct timespec times[2], struct fuse_file_info *fi)
{
    (void)fi;
    struct extent *extent = extent_for(path);
    if (!extent) return -ENOENT;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return -errno;
    for (size_t i = 0; i < 2; ++i)
        if (times[i].tv_nsec != UTIME_OMIT)
            extent->times[i] = times[i].tv_nsec == UTIME_NOW ? now : times[i];
    return 0;
}

static int ram_sync(const char *p, int datasync, struct fuse_file_info *fi)
{
    (void)p; (void)datasync; (void)fi;
    /* All preceding FUSE writes have committed to the volatile page store. */
    return 0;
}
static void *initialize(struct fuse_conn_info *conn, struct fuse_config *conf)
{
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    conf->kernel_cache = 1;
    return NULL;
}
static int size_mib(const char *s, size_t *bytes)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || end == s || *end || n > 1048576) return -1;
    *bytes = (size_t)n << 20;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 5 || size_mib(argv[1], &boot.bytes) || !boot.bytes ||
        size_mib(argv[2], &dimm.bytes) || sizeof(size_t) < 8 || sysconf(_SC_PAGESIZE) != PAGE)
        return 2;
    dimm.first = boot.bytes / PAGE;
    char *end;
    long fd = strtol(argv[3], &end, 10);
    if (end == argv[3] || *end || fd < 0 || fd > INT_MAX) return 2;
    memory = store_new_file((boot.bytes + dimm.bytes) / PAGE, (int)fd);
    close((int)fd);
    if (!memory) return 1;
    const struct fuse_operations ops = { .getattr = attributes, .readdir = list,
        .read = ram_read, .write = ram_write, .fsync = ram_sync, .utimens = ram_times, .init = initialize };
    char *args[] = {argv[0], "-f", "-s", "-o", "default_permissions", argv[4]};
    int rc = fuse_main(6, args, &ops, NULL);
    uint64_t stats[5], disk_reads, disk_writes;
    store_file_stats(memory, &disk_reads, &disk_writes);
    store_stats(memory, stats);
    fprintf(stderr, "VRAM_FILE {\"read_pages\":%" PRIu64 ",\"write_pages\":%" PRIu64
            ",\"file_base_reads\":%" PRIu64 ",\"file_base_writes\":%" PRIu64
            ",\"store_bytes\":%" PRIu64 "}\n", reads, writes, disk_reads, disk_writes, stats[4]);
    store_free(memory);
    return rc;
}
