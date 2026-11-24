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

enum { BACKENDS = 3 };
static const char *paths[] = {"/dev/zram0", "/dev/nbd0", "/dev/nbd1"};
static void finish(int code) { fflush(NULL); outl(code, 0xf4); _exit(code); }
static void fail(const char *op) { perror(op); finish(1); }
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
    setting("/sys/block/zram0/comp_algorithm", "zstd");
    setting("/sys/block/zram0/disksize", "67108864");
    pid_t pid[BACKENDS] = {0};
    for (int i = 1; i < BACKENDS; ++i) {
        pid[i] = fork();
        if (pid[i] < 0) fail("fork");
        const char *server = i == 1 ? "/control" : "/candidate";
        if (!pid[i]) { execl(server, server, "attach", paths[i], "64", "49", NULL); fail("exec server"); }
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) fail(paths[i]);
        uint64_t size = 0;
        for (unsigned tries = 0; size != 67108864 && tries < 3000; ++tries) {
            if (ioctl(fd, BLKGETSIZE64, &size)) fail("device size");
            if (size != 67108864) usleep(10000);
        }
        if (size != 67108864) fail("attach timeout");
        close(fd);
    }
    pid_t workload = fork();
    if (workload < 0) fail("fork workload");
    if (!workload) {
        execl("/usr/bin/python3", "python3", "-IS", "/workload.py", NULL);
        fail("exec benchmark");
    }
    int status;
    if (waitpid(workload, &status, 0) != workload || status) fail("benchmark");
    for (int i = 1; i < BACKENDS; ++i) {
        int fd = open(paths[i], O_RDWR);
        if (fd < 0 || ioctl(fd, NBD_DISCONNECT)) fail("disconnect");
        close(fd);
        int status;
        if (waitpid(pid[i], &status, 0) != pid[i] || status) fail("server exit");
    }
    setting("/sys/block/zram0/reset", "1");
    finish(0);
}
