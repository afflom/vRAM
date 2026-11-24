// SPDX-License-Identifier: MIT OR Apache-2.0
#define _GNU_SOURCE
#include "pager.h"
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/nbd.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

enum { PAGE = 4096 };
struct io_counts { uint64_t writes, reads, trims; };
static void die(const char *name) { perror(name); exit(1); }

#include "managed.h"

static int transfer(int fd, void *buffer, size_t bytes, bool send)
{
    unsigned char *p = buffer;
    while (bytes) {
        ssize_t n = send ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        bytes -= n;
    }
    return 0;
}

static int serve(int fd, struct store *store, size_t pages, struct io_counts *counts)
{
    for (;;) {
        struct nbd_request req;
        if (transfer(fd, &req, sizeof(req), false) || ntohl(req.magic) != NBD_REQUEST_MAGIC)
            return -1;
        uint32_t type = ntohl(req.type) & 0xffff;
        if (type == NBD_CMD_DISC) return 0;
        uint64_t off = be64toh(req.from);
        uint32_t len = ntohl(req.len);
        struct nbd_reply reply = { .magic = htonl(NBD_REPLY_MAGIC) };
        memcpy(reply.handle, req.handle, sizeof(reply.handle));
        uint8_t data[PAGE];
        if (type == NBD_CMD_FLUSH) {
            /* Each write has already committed to the volatile Verdant store. */
            if (len) reply.error = htonl(EINVAL);
            if (transfer(fd, &reply, sizeof(reply), true)) return -1;
            continue;
        }
        if (!vram_range(off, len, pages * PAGE)) {
            /* Consume the rejected payload to preserve framing for later I/O. */
            while (type == NBD_CMD_WRITE && len) {
                uint32_t chunk = len < PAGE ? len : PAGE;
                if (transfer(fd, data, chunk, false)) return -1;
                len -= chunk;
            }
            reply.error = htonl(EINVAL);
            if (transfer(fd, &reply, sizeof(reply), true)) return -1;
            continue;
        }
        size_t begin = off / PAGE, end = begin + len / PAGE;
        if (type == NBD_CMD_READ) {
            if (transfer(fd, &reply, sizeof(reply), true)) return -1;
            for (size_t i = begin; i < end; ++i) {
                if (store_get(store, i, data) || transfer(fd, data, PAGE, true)) return -1;
                counts->reads++;
            }
            continue;
        }
        if (type == NBD_CMD_TRIM) {
            if (store_discard(store, begin, end - begin)) return -1;
            counts->trims += end - begin;
        } else if (type == NBD_CMD_WRITE) {
            for (size_t i = begin; i < end; ++i) {
                if (transfer(fd, data, PAGE, false)) return -1;
                if (store_put(store, i, data)) return -1;
                counts->writes++;
            }
        } else {
            reply.error = htonl(EOPNOTSUPP);
        }
        if (transfer(fd, &reply, sizeof(reply), true)) return -1;
    }
}

struct kernel_loop { int device, socket, error; };
static void *drive_kernel(void *arg)
{
    struct kernel_loop *loop = arg;
    if (ioctl(loop->device, NBD_DO_IT) < 0 && errno != EPIPE) {
        loop->error = errno;
        shutdown(loop->socket, SHUT_RDWR);
    }
    return NULL;
}

static void usage(void)
{
    fputs("vram-host run /dev/nbdN auto|SIZE_MiB [SEARCH_NODES]\nvram-host attach /dev/nbdN SIZE_MiB [SEARCH_NODES]\nvram-host detach /dev/nbdN\n", stderr);
    exit(2);
}
int main(int argc, char **argv)
{
    bool managed = (argc == 4 || argc == 5) && !strcmp(argv[1], "run");
    bool attach = managed || ((argc == 4 || argc == 5) && !strcmp(argv[1], "attach"));
    if (!attach && (argc != 3 || strcmp(argv[1], "detach"))) usage();
    int fd = open(argv[2], O_RDWR | O_CLOEXEC | (attach ? 0 : O_EXCL));
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) die(argv[2]);
    if (!S_ISBLK(st.st_mode) || major(st.st_rdev) != 43) usage();
    if (!attach) {
        if (ioctl(fd, NBD_DISCONNECT)) die("NBD_DISCONNECT");
        return 0;
    }
    if (sysconf(_SC_PAGESIZE) != PAGE) {
        fputs("vRAM requires 4 KiB host pages.\n", stderr);
        return 1;
    }
    char *tail;
    errno = 0;
    unsigned long mib = strtoul(argv[3], &tail, 10);
    if (managed && !strcmp(argv[3], "auto")) {
        mib = available_mib();
        tail = argv[3] + strlen(argv[3]);
        errno = 0;
    }
    if (errno || *tail || !mib || mib > 1048576 || sizeof(size_t) < 8) usage();
    unsigned long nodes = 49;
    if (argc == 5) {
        errno = 0;
        nodes = strtoul(argv[4], &tail, 10);
        if (errno || tail == argv[4] || *tail || nodes > 49) usage();
    }
    FILE *zswap = fopen("/sys/module/zswap/parameters/enabled", "r");
    if (zswap) {
        int enabled = fgetc(zswap);
        fclose(zswap);
        if (enabled != 'N' && enabled != '0') {
            fputs("Disable Linux zswap before attaching Verdant swap.\n", stderr);
            return 1;
        }
    } else if (errno != ENOENT) die("read zswap status");
    size_t pages = mib * 256;
    /* Reclaim must never send the serving process's own store to itself. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE)) die("mlockall");
    struct store *store = store_new_config(pages, nodes, true);
    if (!store) die("Verdant store allocation");
    uint8_t header[PAGE] = {0};
    uint32_t version = 1, last = pages - 1;
    memcpy(header + 1024, &version, 4);
    memcpy(header + 1028, &last, 4);
    memcpy(header + PAGE - 10, "SWAPSPACE2", 10);
    if (store_put(store, 0, header)) die("swap header");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) die("socketpair");
    if (ioctl(fd, NBD_SET_SOCK, pair[1])) die("NBD_SET_SOCK");
    if (ioctl(fd, NBD_SET_BLKSIZE, PAGE) || ioctl(fd, NBD_SET_SIZE_BLOCKS, pages) ||
        ioctl(fd, NBD_SET_FLAGS, NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_TRIM | NBD_FLAG_SEND_FLUSH)) {
        perror("configure NBD"); ioctl(fd, NBD_CLEAR_SOCK); return 1;
    }
    close(pair[1]);
    /* Sharing the locked address space prevents a forked ioctl worker from
       retaining copy-on-write snapshots of the store during reclaim. */
    struct kernel_loop loop = { .device = fd, .socket = pair[0] };
    struct managed control = { .device = fd, .socket = pair[0], .path = argv[2], .store = store,
                               .capacity = pages * PAGE };
    pthread_t worker, manager;
    if (managed) {
        sigemptyset(&control.signals);
        sigaddset(&control.signals, SIGINT);
        sigaddset(&control.signals, SIGTERM);
        sigaddset(&control.signals, SIGUSR1);
        int rc = pthread_sigmask(SIG_BLOCK, &control.signals, NULL);
        if (rc) { ioctl(fd, NBD_CLEAR_SOCK); errno = rc; die("block management signals"); }
    }
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    if (!error) {
        error = pthread_attr_setstacksize(&attr, 128 * 1024);
        if (!error) error = pthread_create(&worker, &attr, drive_kernel, &loop);
        pthread_attr_destroy(&attr);
    }
    if (error) {
        ioctl(fd, NBD_CLEAR_SOCK);
        errno = error;
        die("start NBD kernel worker");
    }
    if (managed) {
        error = pthread_create(&manager, NULL, manage_swap, &control);
        if (error) {
            shutdown(pair[0], SHUT_RDWR);
            pthread_join(worker, NULL);
            ioctl(fd, NBD_CLEAR_SOCK);
            errno = error;
            die("start swap activation");
        }
    }
    signal(SIGPIPE, SIG_IGN);
    printf("Serving %lu MiB of volatile Verdant swap on %s\n", mib, argv[2]);
    fflush(stdout);
    struct io_counts counts = {0};
    int rc = serve(pair[0], store, pages, &counts);
    shutdown(pair[0], SHUT_RDWR);
    error = pthread_join(worker, NULL);
    if (error) { errno = error; die("join NBD kernel worker"); }
    if (managed) {
        pthread_kill(manager, SIGTERM);
        pthread_join(manager, NULL);
    }
    close(pair[0]);
    if (loop.error) { errno = loop.error; perror("NBD_DO_IT"); }
    ioctl(fd, NBD_CLEAR_SOCK);
    uint64_t stats[5], modeled, restores;
    store_stats(store, stats);
    store_selector_stats(store, &modeled, &restores);
    printf("VRAM_STORE {\"write_pages\":%lu,\"read_pages\":%lu,\"trim_pages\":%lu,"
           "\"raw_pages\":%lu,\"selector_pages\":%lu,\"selector_restores\":%lu,\"store_bytes\":%lu}\n",
           counts.writes, counts.reads, counts.trims, stats[2], modeled, restores, stats[4]);
    report_store(store);
    store_free(store);
    return rc || loop.error || control.error;
}
