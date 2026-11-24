// A destructive, disposable initramfs PID 1 workload. All devices are created in this guest.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nbd.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint32_t u32;
enum { PAGE = 4096, SAMPLES = 1024, BACKENDS = 3 };
#include "../../device/validation/capacity_pattern.h"
static const char *paths[] = {"/dev/zram0", "/dev/nbd0", "/dev/nbd1"};
static const char *names[] = {"zram-zstd", "control", "vram"};
static u8 corpus[SAMPLES][PAGE] __attribute__((aligned(PAGE)));
static u8 restored[PAGE] __attribute__((aligned(PAGE)));
static u32 order[SAMPLES];
static uint64_t durations[SAMPLES];

static void finish(int code) { fflush(NULL); outl(code, 0xf4); _exit(code); }
static void fail(const char *op) { perror(op); finish(1); }
static void pin(unsigned cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask)) fail("CPU affinity");
}
static void load(const char *name, const char *options) {
    int fd = open(name, O_RDONLY);
    if (fd < 0 || syscall(SYS_finit_module, fd, options, 0)) fail(name);
    close(fd);
}
static void setting(const char *name, const char *value) {
    int fd = open(name, O_WRONLY);
    if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value)) fail(name);
    close(fd);
}
static uint64_t now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) fail("clock");
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void measure(int backend, int round, char profile) {
    int fd = open(paths[backend], O_RDWR | O_DIRECT);
    if (fd < 0) fail(paths[backend]);
    for (int reading = 0; reading <= 1; ++reading) {
        for (unsigned i = 0; i < SAMPLES; ++i) {
            unsigned at = reading ? order[i] : i;
            off_t off = (off_t)(1 + at + (profile == 'U' ? SAMPLES : 0)) * PAGE;
            uint64_t start = now();
            ssize_t n = reading ? pread(fd, restored, PAGE, off) : pwrite(fd, corpus[at], PAGE, off);
            durations[i] = now() - start;
            if (n != PAGE) fail("direct page I/O");
            if (reading && memcmp(corpus[at], restored, PAGE)) fail("page mismatch");
        }
        qsort(durations, SAMPLES, sizeof(*durations), cmp);
        printf("PAGE_LATENCY %d %s %c %s %llu %llu\n", round, names[backend], profile,
               reading ? "read" : "write", (unsigned long long)((durations[SAMPLES/2-1] +
               durations[SAMPLES/2]) / 2), (unsigned long long)durations[(SAMPLES*99+99)/100-1]);
        fflush(stdout);
    }
    close(fd);
}
int main(void) {
    if (getpid() != 1 || ioperm(0xf4, 4, 1)) return 2;
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) || mount("proc", "/proc", "proc", 0, NULL))
        fail("mount");
    load("/lz4_compress.ko", "");
    load("/lz4hc_compress.ko", "");
    load("/zram.ko", "num_devices=1");
    load("/nbd.ko", "nbds_max=2 max_part=0");
    unsigned major_num, minor_num;
    FILE *dev = fopen("/sys/block/zram0/dev", "r");
    if (!dev || fscanf(dev, "%u:%u", &major_num, &minor_num) != 2) fail("zram dev");
    fclose(dev);
    if (mknod(paths[0], S_IFBLK | 0600, makedev(major_num, minor_num))) fail("zram node");
    for (int i = 1; i < BACKENDS; ++i)
        if (mknod(paths[i], S_IFBLK | 0600, makedev(43, i-1))) fail("nbd node");
    struct rlimit lock = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &lock)) fail("memlock");
    pin(0);
    for (int round = 0; round < 3; ++round) {
        paths[1] = round % 2 ? "/dev/nbd1" : "/dev/nbd0";
        paths[2] = round % 2 ? "/dev/nbd0" : "/dev/nbd1";
        setting("/sys/block/zram0/comp_algorithm", "zstd");
        setting("/sys/block/zram0/disksize", "16777216");
        pid_t pid[BACKENDS] = {0};
        for (int i = 1; i < BACKENDS; ++i) {
            pid[i] = fork();
            if (pid[i] < 0) fail("fork");
            const char *server = i == 1 ? "/control" : "/candidate";
            if (!pid[i]) { pin(1); execl(server, server, "attach", paths[i], "16", NULL); fail("exec server"); }
            int fd = open(paths[i], O_RDONLY);
            if (fd < 0) fail(paths[i]);
            uint64_t size = 0;
            for (unsigned tries = 0; size != 16777216 && tries < 3000; ++tries) {
                if (ioctl(fd, BLKGETSIZE64, &size)) fail("device size");
                if (size != 16777216) usleep(10000);
            }
            if (size != 16777216) fail("attach timeout");
            close(fd);
        }
        for (int p = 0; p < 2; ++p) {
            char profile = p ? 'U' : 'S';
            for (unsigned i = 0; i < SAMPLES; ++i) {
                expected(i + round * SAMPLES, profile, corpus[i]);
                order[i] = i;
            }
            u32 seed = 2026 + round;
            for (u32 i = SAMPLES - 1; i; --i) {
                seed = mix(seed + i);
                u32 j = seed % (i+1), t = order[i]; order[i] = order[j]; order[j] = t;
            }
            for (int j = 0; j < BACKENDS; ++j) measure((round+j)%BACKENDS, round, profile);
        }
        for (int i = 1; i < BACKENDS; ++i) {
            int fd = open(paths[i], O_RDWR);
            if (fd < 0 || ioctl(fd, NBD_DISCONNECT)) fail("disconnect");
            close(fd);
            int status;
            if (waitpid(pid[i], &status, 0) != pid[i] || status) fail("server exit");
        }
        setting("/sys/block/zram0/reset", "1");
    }
    finish(0);
}
