/* Freestanding i386 memory exerciser. Commands travel on COM1. The host
 * supplies indices and workload selection only, never generated page data. */
typedef unsigned char u8;
typedef unsigned int u32;
enum { UART = 0x3f8, PAGE = 4096, MIB = 1048576, START = 2 * MIB };

#ifndef TILES
#define TILES 128
#endif

static u8 input(u32 port) {
    u8 value;
    __asm__ volatile("inb %w1,%0" : "=a"(value) : "d"(port));
    return value;
}
static void output(u32 port, u8 value) {
    __asm__ volatile("outb %0,%w1" :: "a"(value), "d"(port));
}
static u8 receive(void) {
    while (!(input(UART + 5) & 1)) __asm__ volatile("pause");
    return input(UART);
}
static void send(u8 value) {
    while (!(input(UART + 5) & 32)) __asm__ volatile("pause");
    output(UART, value);
}
#include "capacity_pattern.h"
void benchmark(void) {
    output(UART + 3, 3);
    output(UART + 1, 0);
    output(UART + 2, 7);
    send('B');
    u8 profile = receive();
    u8 scratch[PAGE] __attribute__((aligned(4)));
    for (;;) {
        u8 op = receive();
        if (op == 'Q') { output(0xf4, 0); for (;;) __asm__ volatile("hlt"); }
        u32 tile = receive();
#if TILES > 128
        tile |= (u32)receive() << 8;
#endif
        if (tile >= TILES || (op != 'W' && op != 'V' && op != 'H')) {
            send('E'); return;
        }
        for (u32 i = 0; i < MIB / PAGE; i++) {
            u32 page = tile * (MIB / PAGE) + i;
            volatile u32 *ram = (volatile u32 *)(START + page * PAGE);
            if (op == 'H') {
                u32 sample = *ram;
                (void)sample;
                continue;
            }
            expected(page, profile, scratch);
            for (u32 k = 0; k < PAGE / 4; k++) {
                u32 word;
                __builtin_memcpy(&word, scratch + 4 * k, 4);
                if (op == 'W') ram[k] = word;
                else if (ram[k] != word) { send('E'); return; }
            }
        }
        send('D');
    }
}
