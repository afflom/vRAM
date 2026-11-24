/* Optional AMD SVM modules inside the disposable outer Linux test machine. */
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
static int load_svm_modules(void) {
    const char *mods[] = {"/irqbypass.ko", "/ccp.ko", "/kvm.ko", "/kvm-amd.ko"};
    for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        int fd = open(mods[i], O_RDONLY);
        if (fd < 0) return -1;
        int result = syscall(SYS_finit_module, fd, "", 0);
        close(fd);
        if (result) return -1;
    }
    return 0;
}
