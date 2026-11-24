/* Called after the test has verified placement of the operand on the DIMM. */
#define _GNU_SOURCE
#include <vram/projection.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, LANES = 64, TRIALS = 24 };

static int open_port(void)
{
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) && errno != EBUSY) return -1;
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) && errno != EBUSY) return -1;
    glob_t ports = {0};
    int fd = -1;
    if (!glob("/sys/class/virtio-ports/*/name", 0, NULL, &ports)) {
        for (size_t i = 0; i < ports.gl_pathc; ++i) {
            char name[80], path[96];
            FILE *file = fopen(ports.gl_pathv[i], "r");
            if (!file) continue;
            int fields = fscanf(file, "%79s", name);
            fclose(file);
            if (fields != 1 || strcmp(name, VRAM_PROJECT_PORT)) continue;
            if (sscanf(ports.gl_pathv[i], "/sys/class/virtio-ports/%79[^/]/name", name) != 1) break;
            snprintf(path, sizeof(path), "/dev/%s", name);
            fd = open(path, O_RDWR);
            break;
        }
    }
    globfree(&ports);
    return fd;
}

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static __attribute__((noinline)) void dense(const uint8_t *src, const int32_t *w, int64_t *out)
{
    memset(out, 0, LANES * sizeof(*out));
    for (int r = 0; r < LANES; ++r)
        for (int c = 0; c < LANES; ++c)
            out[c] += (int64_t)w[r] * src[r * LANES + c];
}

int projection_trial(uint8_t *page, uint64_t offset, uint64_t extent)
{
    int fd = open_port();
    if (fd < 0) { perror("projection port"); return -1; }
    struct vram_request q = {.operation = VRAM_PROJECT, .offset = offset};
    int64_t got[LANES], oracle[LANES];
    uint8_t restored[PAGE];
    /* Only complete frames are sent. A refusal must leave the stream usable. */
    q.offset = extent;
    if (vram_projection_call(fd, &q, got) != -EINVAL) return -1;
    q.offset = offset + 1;
    if (vram_projection_call(fd, &q, got) != -EINVAL) return -1;
    q.offset = offset;
    q.operation = 99;
    if (vram_projection_call(fd, &q, got) != -EINVAL) return -1;
    q.operation = VRAM_PROJECT;
    q.reserved = 1;
    if (vram_projection_call(fd, &q, got) != -EINVAL) return -1;
    q.reserved = 0;
    /* The original sample has an independent literal or a shared literal with edits. */
    q.weights[7] = INT32_MIN;
    dense(page, q.weights, oracle);
    if (vram_projection_call(fd, &q, got) != 0 || memcmp(got, oracle, sizeof(got))) return -1;

    uint32_t state = 67671;
    for (size_t i = 0; i < PAGE; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        page[i] = (state & 15) + 80;
    }
    for (unsigned t = 0; t < TRIALS; ++t) {
        page[1591] = 202 + t; /* Outside the palette. Check publication of each new version. */
        for (int r = 0; r < LANES; ++r)
            q.weights[r] = (r & 1 ? -r : r) * (int)(t + 1);
        q.weights[t] = (t & 1) ? INT32_MIN : INT32_MAX;
        for (int c = 0; c < LANES; ++c) {
            oracle[c] = 0;
            for (int r = 0; r < LANES; ++r)
                oracle[c] += (int64_t)page[r * LANES + c] * q.weights[r];
        }
        uint64_t elapsed[3];
        for (unsigned j = 0; j < 3; ++j) {
            unsigned route = (t + (t & 1 ? 2 - j : j)) % 3;
            q.operation = route == 1 ? VRAM_RESTORE : VRAM_PROJECT;
            uint64_t start = now();
            if (route == 0) {
                int status = vram_projection_call(fd, &q, got);
                if (status != 1) { printf("projection route %d at trial %u\n", status, t); return -1; }
            } else {
                if (route == 1 && vram_projection_call(fd, &q, restored) != 0) return -1;
                dense(route == 1 ? restored : page, q.weights, got);
            }
            elapsed[route] = now() - start;
            if (!elapsed[route] || memcmp(got, oracle, sizeof(got)) ||
                (route == 1 && memcmp(page, restored, PAGE))) return -1;
        }
        printf("PROJECTION_TRIAL {\"trial\":%u,\"packed_ns\":%" PRIu64 ",\"restore_cpu_ns\":%" PRIu64
               ",\"resident_cpu_ns\":%" PRIu64 "}\n", t, elapsed[0], elapsed[1], elapsed[2]);
    }
    close(fd);
    return 0;
}
