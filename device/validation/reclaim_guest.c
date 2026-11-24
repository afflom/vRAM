/* PID 1 of the inner guest. Physical addresses are reported to target this
 * allocation precisely. All data accesses are ordinary loads and stores. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <termios.h>
#include <unistd.h>

enum { PAGE = 4096, MAX_PAGES = 1024 };
#ifdef SELECTOR_TEST
enum { ALLOC_PAGES = 4096 };
static const unsigned pages[] = { 2, 256, MAX_PAGES, MAX_PAGES, MAX_PAGES, MAX_PAGES,
                                  ALLOC_PAGES, ALLOC_PAGES };
#else
enum { ALLOC_PAGES = MAX_PAGES };
static const unsigned pages[] = { 2, 256, MAX_PAGES, MAX_PAGES, MAX_PAGES, MAX_PAGES };
#endif
static void die(const char *reason)
{
    dprintf(1, "FAIL %s errno=%d\n", reason, errno);
    outb(1, 0xf4);
    _exit(1);
}
static uint8_t initial(unsigned round, uint32_t i)
{
    if (round >= 6) {
        uint32_t page = i / PAGE;
        uint32_t column = i % 64;
        uint8_t row = (i % PAGE) / 64;
        uint8_t a = row * 3 + page;
        uint32_t bit = ((column * 13 + page) & 7) < 3;
        return a ^ (bit ? 0x55 : 0);
    }
    if (round == 0 || round == 2) return 0x85;
    if (round == 1 || round == 3) return (i ^ (i >> 8)) & 15;
    uint32_t x = i + 0x7f4a7c15u;
    x = (x ^ (x >> 16)) * 0x21f0aaadu;
    x = (x ^ (x >> 15)) * 0x735a2d97u;
    return (uint8_t)(x ^ (x >> 15));
}
static uint32_t marker(unsigned round, unsigned boundary)
{
    return 0x76543210u ^ (0x11111111u * round) ^ (boundary / PAGE);
}
static void send_ranges(uint8_t *mem, unsigned count, int map)
{
    uint64_t base = 0, next = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t entry;
        off_t off = ((uintptr_t)mem / PAGE + i) * sizeof(entry);
        if (pread(map, &entry, sizeof entry, off) != sizeof entry ||
            !(entry >> 63)) die("missing guest frame");
        uint64_t addr = (entry & ((1ull << 55) - 1)) * PAGE;
        /* On this 192 MiB PC the allocated DRAM above 1 MiB has identical
         * guest physical and main-backend offsets. */
        if (addr < 0x100000 || addr >= (192ull << 20)) die("guest frame");
        if (i && next != addr) printf("R %lu %lu\n", base, next - base);
        if (!i || next != addr) base = addr;
        next = addr + PAGE;
    }
    printf("R %lu %lu\n", base, next - base);
}
static void await_resume(void)
{
    char ch;
    if (read(0, &ch, 1) != 1 || ch != 'G') die("serial acknowledgement");
}
int main(void)
{
    int tty = open("/dev/ttyS0", O_RDWR);
    if (tty < 0 || ioperm(0xf4, 1, 1)) return 1;
    struct termios t;
    if (tcgetattr(tty, &t)) die("get terminal");
    cfmakeraw(&t);
    if (tcsetattr(tty, TCSANOW, &t)) die("set terminal");
    dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (mount("proc", "/proc", "proc", 0, NULL)) die("mount proc");
    int map = open("/proc/self/pagemap", O_RDONLY);
    size_t length = ALLOC_PAGES * PAGE;
    uint8_t *mem = mmap(NULL, length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map < 0 || mem == MAP_FAILED || madvise(mem, length, MADV_NOHUGEPAGE) ||
        mlock(mem, length)) die("allocate and lock guest RAM");
    for (unsigned round = 0; round < sizeof pages / sizeof pages[0]; round++) {
        unsigned bytes = pages[round] * PAGE;
        /* The last round changes only boundary words in the restored region. */
        if (round == 7) {
            for (unsigned i = 17; i < bytes; i += 17 * PAGE) mem[i] ^= 0x80;
        }
        if (round != 5 && round != 7)
            for (unsigned i = 0; i < bytes; i++) mem[i] = initial(round, i);
        for (unsigned b = PAGE; round < 6 && b < bytes; b += PAGE) {
            uint32_t word = marker(round, b);
            __asm__ volatile("movl %1,(%0)" :: "r"(mem + b - 2), "r"(word) : "memory");
        }
        send_ranges(mem, pages[round], map);
        printf("READY %u %u\n", round, pages[round]);
        await_resume();
        const volatile uint8_t *v = mem;
        for (unsigned i = 0; i < bytes; i++) {
            uint8_t expected = initial(round, i);
            unsigned b = (i + 2) & ~(PAGE - 1u);
            if (round == 7 && i % (17 * PAGE) == 17) expected ^= 0x80;
            if (round < 6 && b && b < bytes && i + 2 - b < 4)
                expected = marker(round, b) >> (8 * (i + 2 - b));
            if (v[i] != expected) die("post-resume byte mismatch");
        }
        printf("VERIFIED %u %u\n", round, bytes);
        await_resume();
    }
    puts("GUEST PASS");
    tcdrain(1);
    outb(0, 0xf4);
    return 0;
}
