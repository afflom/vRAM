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
static unsigned affinity(const char *path) {
    FILE *file = fopen(path, "r");
    unsigned value;
    if (!file || fscanf(file, "%u", &value) != 1 || value > 2) fail(path);
    fclose(file);
    return value;
}

static void block_node(const char *sysfs, const char *node) {
    unsigned major_num, minor_num;
    FILE *dev = fopen(sysfs, "r");
    if (!dev || fscanf(dev, "%u:%u", &major_num, &minor_num) != 2) fail(sysfs);
    fclose(dev);
    if (mknod(node, S_IFBLK | 0600, makedev(major_num, minor_num))) fail(node);
}

int main(void) {
    if (getpid() != 1 || ioperm(0xf4, 4, 1)) return 2;
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) || mount("proc", "/proc", "proc", 0, NULL))
        fail("mount");
    load("/lz4_compress.ko", "");
    load("/lz4hc_compress.ko", "");
    load("/zram.ko", "num_devices=1");
    int literal = access("/kernel-literal.ko", F_OK) == 0;
    int fill = access("/kernel-fill.ko", F_OK) == 0;
    if (!fill) load("/nbd.ko", "nbds_max=2 max_part=0");
    int nbd_end = fill ? 1 : (literal ? 2 : BACKENDS);
    block_node("/sys/block/zram0/dev", paths[0]);
    if (literal) {
        load("/kernel-literal.ko", "");
        block_node("/sys/block/vramlit0/dev", "/dev/vramlit0");
    }
    if (fill) {
        load("/kernel-fill.ko", "");
        block_node("/sys/block/vramfill0/dev", "/dev/vramfill0");
    }
    for (int i = 1; i < nbd_end; ++i)
        if (mknod(paths[i], S_IFBLK | 0600, makedev(43, i-1))) fail("nbd node");
    struct rlimit lock = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &lock)) fail("memlock");
    setting("/sys/block/zram0/comp_algorithm", "zstd");
    setting("/sys/block/zram0/disksize", "67108864");
    pid_t pid[BACKENDS] = {0};
    for (int i = 1; i < nbd_end; ++i) {
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
    if (access("/local-completion", F_OK) == 0) {
        const char *candidate = "/sys/block/nbd1/queue/rq_affinity";
        const char *reference = "/sys/block/nbd0/queue/rq_affinity";
        unsigned control = affinity(reference);
        if (affinity(candidate) != control) fail("initial affinity mismatch");
        setting(candidate, "0");
        if (affinity(reference) != control || affinity(candidate) != 0) fail("completion setting mismatch");
        printf("COMPLETION_AFFINITY %u %u\n", control, affinity(candidate));
        fflush(stdout);
    }
    pid_t workload = fork();
    if (workload < 0) fail("fork workload");
    if (!workload) {
        execl("/usr/bin/python3", "python3", "-IS", "/workload.py", NULL);
        fail("exec benchmark");
    }
    int status;
    if (waitpid(workload, &status, 0) != workload || status) fail("benchmark");
    if (fill) {
        if (unlink("/dev/vramfill0") || syscall(SYS_delete_module, "vram_fill", O_NONBLOCK)) fail("unload fill");
        if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) fail("activation devtmpfs");
        pid_t activation = fork();
        if (activation < 0) fail("fork activation");
        if (!activation) {
            const char *mounts[] = {"/dev", "/sys", "/proc"};
            for (size_t i = 0; i < 3; i++) {
                char target[64];
                snprintf(target, sizeof(target), "/kernel-system%s", mounts[i]);
                if (mkdir(target, 0755) || mount(mounts[i], target, NULL, MS_BIND, NULL)) fail(target);
            }
            if (chroot("/kernel-system") || chdir("/") || setenv("PATH", "/bin:/sbin", 1) ||
                setenv("VRAM_KERNEL_SIZE_MIB", "96", 1)) fail("activation root");
            execl("/usr/lib/vram/start-kernel-swap", "start-kernel-swap", NULL);
            fail("exec packaged activation");
        }
        if (waitpid(activation, &status, 0) != activation || status) fail("packaged activation");
        pid_t reclaim = fork();
        if (reclaim < 0) fail("fork reclaim");
        if (!reclaim) {
            execl("/kernel-reclaim", "kernel-reclaim", NULL);
            fail("exec reclaim");
        }
        if (waitpid(reclaim, &status, 0) != reclaim || status) fail("kernel swap reclaim");
    }
    if (access("/store-clock", F_OK) == 0) {
        const unsigned port = 0x505;
        int fd = open("/path-samples.json", O_RDONLY);
        if (fd < 0 || ioperm(port, 1, 1)) fail("trace export");
        char buf[4096];
        ssize_t count;
        while ((count = read(fd, buf, sizeof(buf))) > 0) outsb(port, buf, (size_t)count);
        if (count < 0) fail("read trace");
        close(fd);
    }
    for (int i = 1; i < nbd_end; ++i) {
        int fd = open(paths[i], O_RDWR);
        if (fd < 0 || ioctl(fd, NBD_DISCONNECT)) fail("disconnect");
        close(fd);
        int status;
        if (waitpid(pid[i], &status, 0) != pid[i] || status) fail("server exit");
    }
    setting("/sys/block/zram0/reset", "1");
    finish(0);
}
