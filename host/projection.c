// SPDX-License-Identifier: MIT OR Apache-2.0
#define _GNU_SOURCE
#include "projection_ioctl.h"
#include "pager.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t transfer(int fd, void *buf, size_t len, bool send)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = send ? write(fd, (char *)buf + done, len - done)
                         : read(fd, (char *)buf + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        done += n;
    }
    return done;
}

/* A zero acknowledgement admits the frame. No trailing weights have been
 * sent at that point, so older brokers and rejected requests stay in sync. */
static int serve_batch(int fd, const struct vram_request *q, uint64_t extent)
{
    uint32_t remaining = q->reserved;
    int32_t status = -EINVAL;
    if (remaining && vram_range(q->offset, 4096, extent))
        status = fsync(fd) ? -EIO : 0;
    if (transfer(STDOUT_FILENO, &status, sizeof(status), true) != sizeof(status)) return 1;
    if (status) return 0;
    struct vram_batch_ioctl tile = {.header.offset = q->offset};
    memcpy(tile.data.weights[0], q->weights, sizeof(q->weights));
    uint32_t skip = 1;
    while (remaining) {
        uint32_t n = remaining < VRAM_PROJECTION_TILE ? remaining : VRAM_PROJECTION_TILE;
        tile.header.rows = n;
        size_t bytes = (n - skip) * sizeof(tile.data.weights[0]);
        if (transfer(STDIN_FILENO, tile.data.weights + skip, bytes, false) != (ssize_t)bytes) return 1;
        int route = ioctl(fd, VRAM_BATCH_IOCTL(n), &tile);
        status = (route == 0 || route == 1) ? route : -EIO;
        if (transfer(STDOUT_FILENO, &status, sizeof(status), true) != sizeof(status)) return 1;
        if (status < 0) return 0;
        bytes = n * sizeof(tile.data.columns[0]);
        if (transfer(STDOUT_FILENO, tile.data.columns, bytes, true) != (ssize_t)bytes) return 1;
        remaining -= n;
        skip = 0;
    }
    return 0;
}

int serve_projection(const char *dimm)
{
    int fd = open(dimm, O_RDWR | O_DIRECT | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) return 1;
    _Alignas(4096) uint8_t restored[4096];
    struct vram_projection query = {0};
    for (;;) {
        struct vram_request *request = &query.request;
        ssize_t n = transfer(STDIN_FILENO, request, sizeof(*request), false);
        if (n == 0) return 0;
        if (n != sizeof(*request)) return 1;
        if (request->operation == VRAM_PROJECT_BATCH) {
            if (serve_batch(fd, request, st.st_size)) return 1;
            continue;
        }
        bool project = request->operation == VRAM_PROJECT;
        int32_t status = -EINVAL;
        if ((project || request->operation == VRAM_RESTORE) && !request->reserved &&
            vram_range(request->offset, sizeof(restored), st.st_size)) {
            status = -EIO;
            /* Flush guest mmap writes before consulting the committed model. */
            if (!fsync(fd)) {
                if (project) {
                    int route = ioctl(fd, VRAM_PROJECT_IOCTL, &query);
                    if (route == 0 || route == 1) status = route;
                } else if (pread(fd, restored, sizeof(restored), request->offset) == sizeof(restored)) {
                    status = 0;
                }
            }
        }
        if (transfer(STDOUT_FILENO, &status, sizeof(status), true) != sizeof(status)) return 1;
        if (status >= 0) {
            void *payload = project ? (void *)query.columns : (void *)restored;
            size_t size = project ? sizeof(query.columns) : sizeof(restored);
            if (transfer(STDOUT_FILENO, payload, size, true) != (ssize_t)size) return 1;
        }
    }
}
