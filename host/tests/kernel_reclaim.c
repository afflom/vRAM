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
static long counter(const char *path)
{
	FILE *f = fopen(path, "r");
	long value;
	if (!f || fscanf(f, "%ld", &value) != 1) fail(path);
	fclose(f);
	return value;
}
static void expected(unsigned i, unsigned char *page)
{
	memset(page, (i % 254) + 1, PAGE);
	if (i % 4 == 0) {
		for (unsigned j = 0; j < PAGE; j += 64) memcpy(page + j, &i, sizeof(i));
	}
	if (i % 2) memcpy(page + PAGE - sizeof(i), &i, sizeof(i));
}

static unsigned long active_swap_kib(void)
{
	FILE *f = fopen("/proc/swaps", "r");
	char line[512], path[256];
	unsigned long kib;
	struct stat wanted, active;

	if (stat(device, &wanted)) fail(device);
	if (!f) fail("read /proc/swaps");
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%255s %*s %lu", path, &kib) == 2 &&
		    !stat(path, &active) && S_ISBLK(active.st_mode) && active.st_rdev == wanted.st_rdev) {
			fclose(f);
			return kib;
		}
	}
	fail("vRAM missing from /proc/swaps");
	return 0;
}

int main(void)
{
	unsigned long active = active_swap_kib();
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
	long peak = counter("/sys/module/vram_fill/parameters/literal_pages");
	long blocks = counter("/sys/module/vram_fill/parameters/repeated_bytes");
	if (blocks < PAGES / 8 * 64) fail("insufficient folded swap reclaim");
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
	long final = counter("/sys/module/vram_fill/parameters/literal_pages");
	long final_blocks = counter("/sys/module/vram_fill/parameters/repeated_bytes");
	if (final != 1 || final_blocks) fail("retired literals still resident");
	printf("KERNEL_RECLAIM {\"capacity_mib\":96,\"active_swap_kib\":%lu,\"verified_bytes\":%u,\"evicted_pages\":%u,"
	       "\"literal_pages_under_pressure\":%ld,\"literal_pages_after_drain\":%ld,\"repeated_bytes_under_pressure\":%ld,\"repeated_bytes_after_drain\":%ld}\n",
	       active, PAGES * PAGE, PAGES - present, peak, final, blocks, final_blocks);
	return 0;
}
