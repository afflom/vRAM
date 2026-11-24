typedef unsigned int u32;
static void finish(unsigned char code) {
    __asm__ volatile("outb %0,$0xf4" :: "a"(code));
    for (;;) __asm__ volatile("hlt");
}
static u32 digest(u32 addr, u32 len) {
    u32 h=2166136261u;
    for (u32 i=0;i<len;i++) h=(h^*(volatile unsigned char *)(addr+i))*16777619u;
    return h;
}
void benchmark(void) {
    if (digest(2u<<20,DIGITS_BYTES)!=DIGITS_HASH) finish(4);
    if (digest(4u<<20,4u<<20)!=KERNEL_HASH) finish(5);
    u32 boundary=(2u<<20)+4094;
    __asm__ volatile("movl $0x44332211,(%0)" :: "r"(boundary) : "memory");
    u32 readback;
    __asm__ volatile("movl (%1),%0" : "=r"(readback) : "r"(boundary) : "memory");
    if (readback!=0x44332211) finish(6);
    for (u32 addr=0x900000; addr<0xa00000; addr+=4096) {
        if (*(volatile u32*)addr) finish(2);
        *(volatile u32*)addr = addr ^ 0x3f7e0000u;
    }
    for (u32 addr=0x900000; addr<0xa00000; addr+=4096)
        if (*(volatile u32*)addr != (addr ^ 0x3f7e0000u) || *(volatile u32*)(addr+4))
            finish(3);
    finish(0);
}
