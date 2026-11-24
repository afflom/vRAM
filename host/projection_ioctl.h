#ifndef VRAM_PROJECTION_IOCTL_H
#define VRAM_PROJECTION_IOCTL_H
#include "projection.h"
#include <sys/ioctl.h>
struct vram_projection {
    struct vram_request request;
    int64_t columns[64];
};
#define VRAM_PROJECT_IOCTL _IOWR('V', 0x51, struct vram_projection)
struct vram_batch_header {
    uint64_t offset;
    uint32_t rows;
    uint32_t reserved;
};
struct vram_batch_ioctl {
    struct vram_batch_header header;
    union {
        int32_t weights[VRAM_PROJECTION_TILE][64];
        int64_t columns[VRAM_PROJECTION_TILE][64];
    } data;
};
#define VRAM_BATCH_IOCTL(rows) _IOC(_IOC_READ | _IOC_WRITE, 'V', 0x52, \
    sizeof(struct vram_batch_header) + (rows) * sizeof(int64_t[64]))
_Static_assert(sizeof(struct vram_batch_ioctl) <= _IOC_SIZEMASK &&
               sizeof(struct vram_batch_ioctl) + sizeof(int64_t[64]) > _IOC_SIZEMASK,
               "wire tile must fit the amd64 ioctl size field");
int serve_projection(const char *dimm);
#endif
