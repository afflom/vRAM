// SPDX-License-Identifier: MIT OR Apache-2.0
#define _GNU_SOURCE
#include "projection_ioctl.h"
#include "pager.h"
#include <errno.h>
#include <fcntl.h>
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
