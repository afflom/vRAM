// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef VRAM_PROJECTION_H
#define VRAM_PROJECTION_H
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

/* Linux amd64, little-endian. One writer owns this byte stream. */
#define VRAM_PROJECT_PORT "org.verdant.project.1"
enum { VRAM_PROJECT = 1, VRAM_RESTORE = 2 };
struct vram_request {
    uint32_t operation;
    uint32_t reserved;
    uint64_t offset;            /* byte offset within the hotplug DIMM */
    int32_t weights[64];
};
_Static_assert(sizeof(struct vram_request) == 272, "projection wire format");
/* Reply starts with an i32: negative errno, 0, or 1 for packed contraction.
 * On success read 64 i64 columns for PROJECT or 4096 bytes for RESTORE. */

/* Blocking calls require exclusive ownership of the port until the full reply.
 * The output has 64 int64_t entries for PROJECT or 4096 bytes for RESTORE. */
static inline int vram_projection_transfer(int fd, void *p, size_t n, int send)
{
    while (n) {
        ssize_t used = send ? write(fd, p, n) : read(fd, p, n);
        if (used < 0 && errno == EINTR) continue;
        if (used <= 0) return -EIO;
        n -= used;
        p = (char *)p + used;
    }
    return 0;
}

static inline int vram_projection_call(int fd, const struct vram_request *request, void *out)
{
    struct vram_request q = *request;
    int32_t status;
    if (vram_projection_transfer(fd, &q, sizeof(q), 1) ||
        vram_projection_transfer(fd, &status, sizeof(status), 0)) return -EIO;
    if (status < 0) return status;
    if (q.operation == VRAM_RESTORE && status == 0)
        return vram_projection_transfer(fd, out, 4096, 0);
    if (q.operation != VRAM_PROJECT || status > 1) return -EPROTO;
    return vram_projection_transfer(fd, out, 64 * sizeof(int64_t), 0) ? -EIO : status;
}
#endif
