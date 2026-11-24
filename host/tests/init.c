#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint32_t u32;
enum { PAGE = 4096, DEFAULT_MIB = WORK_MIB };
#include "../../device/validation/capacity_pattern.h"

static void stop(int code)
{
	fflush(NULL);
	outl(code, 0xf4);
	_exit(code);
}
static void fail(const char *op) { perror(op); stop(1); }
static void control(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value)) fail(path);
	close(fd);
}
static unsigned long vmstat(const char *key)
{
	FILE *f = fopen("/proc/vmstat", "r");
	unsigned long n;
	char label[80];
	if (!f) fail("vmstat");
	while (fscanf(f, "%79s %lu", label, &n) == 2) {
		if (!strcmp(label, key)) { fclose(f); return n; }
	}
	fail(key);
	return 0;
}
static void load(const char *path, const char *args)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0 || syscall(SYS_finit_module, fd, args, 0)) fail(path);
	close(fd);
}

static size_t swapped_pages(const void *mapping, size_t pages)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) fail("open pagemap");
	off_t offset = (uintptr_t)mapping / PAGE * sizeof(uint64_t);
	size_t swapped = 0;
	uint64_t entries[512];
	while (pages) {
		size_t n = pages < 512 ? pages : 512;
		size_t bytes = n * sizeof(*entries);
		ssize_t received;
		do { received = pread(fd, entries, bytes, offset); } while (received < 0 && errno == EINTR);
		if (received != (ssize_t)bytes) fail("read pagemap");
		for (size_t i = 0; i < n; ++i) {
			unsigned state = entries[i] >> 62;
			if (state == 1) ++swapped;
			else if (state != 2) fail("expected present or swapped page");
		}
		pages -= n;
		offset += bytes;
	}
	close(fd);
	return swapped;
}

int main(void)
{
	if (ioperm(0xf4, 4, 1)) return 2;
	if (mount("proc","/proc","proc",0,NULL) || mount("sysfs","/sys","sysfs",0,NULL) ||
	    mount("debugfs","/sys/kernel/debug","debugfs",0,NULL)) fail("mount");
	load("/nbd.ko", "nbds_max=1 max_part=0");
	if (mknod("/dev/nbd0", S_IFBLK | 0600, makedev(43,0))) fail("mknod");
	struct rlimit lock = { RLIM_INFINITY, RLIM_INFINITY };
	if (setrlimit(RLIMIT_MEMLOCK, &lock)) fail("memlock");
	int notify = -1;
	if (MANAGED) {
		notify = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = "/notify" };
		if (notify < 0 || bind(notify, (struct sockaddr *)&addr, sizeof(addr))) fail("notify socket");
	}
	pid_t host = fork();
	if (host < 0) fail("fork");
	if (!host) {
		if (MANAGED) setenv("NOTIFY_SOCKET", "/notify", 1);
		execl("/vram-host", "vram-host", MANAGED ? "run" : "attach", "/dev/nbd0", SWAP_SIZE, NULL);
		fail("exec attach");
	}
	if (MANAGED) {
		struct pollfd pollfd = {.fd=notify,.events=POLLIN};
		char message[160] = {0};
		if (poll(&pollfd, 1, 30000) != 1 || recv(notify, message, sizeof(message)-1, 0) <= 0 ||
		    !strstr(message, "READY=1")) fail("missing readiness");
	} else {
		unsigned long expected_sectors = strtoul(SWAP_SIZE, NULL, 10) * 2048;
		for (unsigned attempt = 0; ; ++attempt) {
			unsigned long sectors;
			FILE *size = fopen("/sys/block/nbd0/size", "r");
			if (!size || fscanf(size, "%lu", &sectors) != 1) fail("read NBD capacity");
			fclose(size);
			if (sectors == expected_sectors) break;
			if (attempt == 200) fail("NBD capacity not ready");
			usleep(50000);
		}
		if (swapon("/dev/nbd0", 0)) fail("swapon");
	}
	if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL)) fail("cgroup2");
	control("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
	if (mkdir("/sys/fs/cgroup/test", 0755)) fail("mkdir cgroup");
	control("/sys/fs/cgroup/test/cgroup.procs", "0");
	size_t pages = DEFAULT_MIB * 256, bytes = pages * PAGE;
	u8 *mem = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED || madvise(mem, bytes, MADV_NOHUGEPAGE)) fail("anonymous mapping");
	control("/sys/fs/cgroup/test/memory.max", "50331648");
	unsigned long writes_before = vmstat("pswpout");
	for (size_t i = 0; i < pages; i++) {
		if (i && !(i % (65536U))) dprintf(1, "written %zu MiB\n", i / 256);
		expected(i, PROFILE, mem + i * PAGE);
	}
	if (madvise(mem, bytes, MADV_PAGEOUT)) fail("pageout");
	/* The mapping's PTEs stay charged even after every data page leaves.
	   Leave room for those PTEs, the residency bitmap and task overhead. */
	char reclaim_limit[32];
	snprintf(reclaim_limit, sizeof(reclaim_limit), "%zu", (4UL << 20) + pages * 10);
	control("/sys/fs/cgroup/test/memory.max", reclaim_limit);
	/* A completed swap write can leave a resident swap-cache copy. Require
	   eviction before counting any read as a Verdant restore. */
	u8 *present = calloc(pages, 1);
	if (!present) fail("mincore allocation");
	size_t resident = 0;
	for (int attempt = 0; attempt < 100; attempt++) {
		if (mincore(mem, bytes, present)) fail("mincore");
		resident = 0;
		for (size_t i = 0; i < pages; i++) resident += present[i] & 1;
		if (resident <= pages / 8) break;
		usleep(50000);
		if (madvise(mem, bytes, MADV_PAGEOUT)) fail("drain swap cache");
	}
	control("/sys/fs/cgroup/test/memory.max", "50331648");
	size_t swapped = swapped_pages(mem, pages);
	unsigned long reads_before = vmstat("pswpin");
	if (MANAGED) { if (kill(host, SIGUSR1)) fail("live stats"); usleep(100000); }
	for (size_t i = 0; i < pages; i++) {
		u8 want[PAGE];
		if (i && !(i % (65536U))) dprintf(1, "verified %zu MiB\n", i / 256);
		expected(i, PROFILE, want);
		if (memcmp(mem + i * PAGE, want, PAGE)) fail("readback");
		if ((!MANAGED || i >= 256) && i % 256 == 255 && madvise(mem + (i - 255) * PAGE, 256 * PAGE, MADV_DONTNEED))
			fail("drop verified tile");
	}
	unsigned long read = vmstat("pswpin") - reads_before;
	unsigned long written = vmstat("pswpout") - writes_before;
	/* Release the verified virtual extent before fork. Linux otherwise
	   charges a potential copy of that entire private mapping to commit. */
	int status;
	if (MANAGED) {
		if (madvise(mem, 256 * PAGE, MADV_PAGEOUT) || kill(host, SIGTERM)) fail("managed stop");
		if (waitpid(host, &status, 0) != host || status) fail("managed drain");
		for (size_t i = 0; i < 256; ++i) {
			u8 want[PAGE];
			expected(i, PROFILE, want);
			if (memcmp(mem+i*PAGE, want, PAGE)) fail("post-stop readback");
		}
		close(notify);
	}
	if (munmap(mem, bytes)) fail("release verified mapping");
	free(present);
	if (!MANAGED) {
		if (swapoff("/dev/nbd0")) fail("swapoff");
		pid_t closer = fork();
		if (closer < 0) fail("fork detach");
		if (!closer) {
			execl("/vram-host", "vram-host", "detach", "/dev/nbd0", NULL);
			fail("exec detach");
		}
		if (waitpid(closer, &status, 0) != closer || status) fail("detach");
		if (waitpid(host, &status, 0) != host || status) fail("host exit");
	}
	printf("VRAM_RESULT {\"verified_bytes\":%zu,\"linux_swap_writes\":%lu,"
	       "\"linux_swap_reads\":%lu,\"swapped_ptes\":%zu,\"present_ptes\":%zu,"
	       "\"resident_or_cached\":%zu}\n", bytes, written, read, swapped, pages - swapped, resident);
	/* A swap PTE can still point at a resident swap-cache page. */
	stop(resident > pages / 8 || swapped < pages * 7 / 8 ||
	     written < pages - resident || read < pages * 3 / 4);
}
