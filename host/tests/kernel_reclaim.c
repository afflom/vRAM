// Runs only in the disposable guest, on a freshly reloaded vram_fill module.
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <unistd.h>

#define PAGE 4096
#define PAGES 6144
static const char *device = "/dev/vramfill0";
static void fail(const char *what) { perror(what); exit(1); }
static void setting(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value)) fail(path);
	close(fd);
}
static long literals(void)
{
	FILE *f = fopen("/sys/module/vram_fill/parameters/literal_pages", "r");
	long value;
	if (!f || fscanf(f, "%ld", &value) != 1) fail("literal_pages");
	fclose(f);
	return value;
}
static void expected(unsigned i, unsigned char *page)
{
	memset(page, (i % 254) + 1, PAGE);
	if (i % 2) memcpy(page + PAGE - sizeof(i), &i, sizeof(i));
}

int main(void)
{
	uint64_t size;
	int fd = open(device, O_RDONLY);
	unsigned char want[PAGE], resident[PAGES];
	if (fd < 0 || ioctl(fd, BLKGETSIZE64, &size) || size != (96U << 20)) fail("capacity");
	close(fd);

	if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL)) fail("cgroup2");
	setting("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
	if (mkdir("/sys/fs/cgroup/reclaim", 0755)) fail("mkdir");
	setting("/sys/fs/cgroup/reclaim/cgroup.procs", "0");
	unsigned char *mem = mmap(NULL, PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED || madvise(mem, PAGES * PAGE, MADV_NOHUGEPAGE)) fail("mapping");
	for (unsigned i = 0; i < PAGES; i++) expected(i, mem + i * PAGE);
	setting("/sys/fs/cgroup/reclaim/memory.max", "5242880");
	unsigned present = PAGES;
	for (int tries = 0; tries < 100 && present > PAGES / 8; tries++) {
		if (madvise(mem, PAGES * PAGE, MADV_PAGEOUT)) fail("pageout");
		usleep(20000);
		if (mincore(mem, PAGES * PAGE, resident)) fail("mincore");
		present = 0;
		for (unsigned i = 0; i < PAGES; i++) present += resident[i] & 1;
	}
	long peak = literals();
	if (present > PAGES / 8 || peak < PAGES / 4) fail("insufficient kernel swap reclaim");
	setting("/sys/fs/cgroup/reclaim/memory.max", "max");
	for (unsigned i = 0; i < PAGES / 2; i++) {
		expected(i, want);
		if (memcmp(mem + i * PAGE, want, PAGE)) fail("fault readback");
	}
	if (swapoff(device)) fail("swapoff");
	for (unsigned i = 0; i < PAGES; i++) {
		expected(i, want);
		if (memcmp(mem + i * PAGE, want, PAGE)) fail("drain readback");
	}
	long final = literals();
	if (final != 1) fail("retired literals still resident");
	printf("KERNEL_RECLAIM {\"capacity_mib\":96,\"verified_bytes\":%u,\"evicted_pages\":%u,"
	       "\"literal_pages_under_pressure\":%ld,\"literal_pages_after_drain\":%ld}\n",
	       PAGES * PAGE, PAGES - present, peak, final);
	return 0;
}
