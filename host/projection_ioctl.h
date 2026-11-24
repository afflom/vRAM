#ifndef VRAM_PROJECTION_IOCTL_H
#define VRAM_PROJECTION_IOCTL_H
#include "projection.h"
#include <sys/ioctl.h>
struct vram_projection {
    struct vram_request request;
    int64_t columns[64];
};
#define VRAM_PROJECT_IOCTL _IOWR('V', 0x51, struct vram_projection)
int serve_projection(const char *dimm);
#endif
