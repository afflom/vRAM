/* Linux PID 1. Check ordinary anonymous pages and count their physical placement. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <termios.h>
#include <unistd.h>

enum { PAGE = 4096 };
static void fail(const char *why)
{
    printf("FAIL %s\n", why);
    reboot(RB_POWER_OFF);
    _exit(1);
}
static unsigned long kib(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) fail("meminfo");
    char line[160], name[48];
    unsigned long value;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%47s %lu", name, &value) == 2 && !strcmp(name, key)) {
            fclose(f);
            return value;
        }
    fail("missing meminfo field");
    return 0;
}
static void contents(uint32_t *words, unsigned page)
{
    uint32_t state = page % 16 == 0 ? page + 1 : 1;
    for (size_t i = 0; i < PAGE / sizeof(*words); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        words[i] = state;
    }
    words[1] = page;
}
int main(void)
{
    int tty = open("/dev/ttyS0", O_RDWR);
    if (tty < 0) return 1;
    struct termios t;
    if (tcgetattr(tty, &t)) return 1;
    cfmakeraw(&t);
    if (tcsetattr(tty, TCSANOW, &t)) return 1;
    dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (mount("proc", "/proc", "proc", 0, NULL)) fail("mount proc");
    unsigned long before = kib("MemTotal:");
    printf("READY %lu\n", before);
    uint64_t address, size;
    if (scanf("%" SCNu64 " %" SCNu64, &address, &size) != 2) fail("DIMM coordinates");
    unsigned long after;
    for (unsigned i = 0;; ++i) {
        after = kib("MemTotal:");
        if (after > before + 9 * (size / 1024) / 10) break;
        if (i == 400) fail("online memory accounting");
        usleep(100000);
    }
    size_t bytes = (before / 1024 + 12) << 20;
    uint8_t *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) fail("mmap");
    unsigned pages = bytes / PAGE;
    for (unsigned i = 0; i < pages; ++i) contents((uint32_t *)(mem + i * PAGE), i);
    int map = open("/proc/self/pagemap", O_RDONLY);
    if (map < 0) fail("pagemap");
    unsigned sample = 0;
    uint64_t within = 0;
    for (; sample < pages; ++sample) {
        uint64_t entry;
        if (pread(map, &entry, sizeof entry, ((uintptr_t)mem / PAGE + sample) * sizeof entry)
            != sizeof entry) fail("sample PFN");
        uint64_t addr = (entry & ((1ull << 55) - 1)) * PAGE;
        if ((entry >> 63) && addr >= address && addr - address < size) {
            within = addr - address;
            break;
        }
    }
    if (sample == pages) fail("no DIMM sample");
    printf("WRITTEN %zu %u %" PRIu64 "\n", bytes, sample, within);
    char ack;
    if (scanf(" %c", &ack) != 1 || ack != 'G') fail("sync acknowledgement");
    unsigned added = 0;
    for (unsigned i = 0; i < pages;) {
        uint64_t entries[256];
        unsigned n = pages - i < 256 ? pages - i : 256;
        off_t offset = ((uintptr_t)mem / PAGE + i) * sizeof(*entries);
        if (pread(map, entries, n * sizeof(*entries), offset) != (ssize_t)(n * sizeof(*entries)))
            fail("read PFNs");
        for (unsigned j = 0; j < n; ++j, ++i) {
            uint64_t addr = (entries[j] & ((1ull << 55) - 1)) * PAGE;
            if (!(entries[j] >> 63) || !addr) fail("page absent");
            if (addr >= address && addr - address < size) ++added;
            uint32_t expected[PAGE / sizeof(uint32_t)];
            contents(expected, i);
            if (memcmp(mem + i * PAGE, expected, PAGE)) fail("byte mismatch");
        }
    }
    if (!added || kib("SwapTotal:")) fail("placement or swap");
    printf("DIMM_RESULT {\"before_kib\":%lu,\"after_kib\":%lu,\"swap_kib\":0,"
           "\"verified_bytes\":%zu,\"verified_on_dimm_bytes\":%zu}\n",
           before, after, bytes, (size_t)added * PAGE);
    tcdrain(1);
    reboot(RB_POWER_OFF);
    return 1;
}
