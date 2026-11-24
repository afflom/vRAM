/* PID 1 in the outer Linux environment. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/io.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef TEST_SVM
#include "svm.h"
#endif
int main(void)
{
    int fd = open("/dev/ttyS0", O_RDWR);
    if (fd < 0 || ioperm(0xf4, 1, 1)) return 1;
    dup2(fd, 1); dup2(fd, 2);
    if (mount("proc", "/proc", "proc", 0, NULL) || mount("sysfs", "/sys", "sysfs", 0, NULL) ||
        mount("none", "/sys/fs/cgroup", "cgroup2", 0, NULL)) {
        perror("mount");
        outb(1, 0xf4);
        return 1;
    }
#ifdef TEST_SVM
    if (load_svm_modules()) {
        perror("loading SVM modules");
        outb(1, 0xf4);
        return 1;
    }
#endif
#ifdef TEST_DIMM
    /* Give the capacity planner a filesystem with a reportable reserve. */
    if (mount("tmpfs", "/tmp", "tmpfs", 0, "size=768m,mode=1777")) {
        perror("mount DIMM test backing");
        outb(1, 0xf4);
        return 1;
    }
#endif
    pid_t pid = fork();
    if (!pid) {
        const char *script = access("/capacity.json", F_OK) == 0 ? "/capacity_manager.py" : "/manage.py";
        execl("/usr/bin/python3", "python3", "-BuS", script, (char *)NULL);
        _exit(127);
    }
    int status;
    int ok = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status);
    outb(ok ? 0 : 1, 0xf4);
    return !ok;
}
