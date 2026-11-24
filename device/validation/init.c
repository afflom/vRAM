#define _GNU_SOURCE
#include "pager.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#ifdef TEST_SVM
#include "svm.h"
#endif
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, SIDE = 64 };
static void finish(int code) {
    /* The debug port terminates QEMU immediately, including pending UART I/O. */
    fflush(stdout);
    tcdrain(STDOUT_FILENO);
    outb(code, 0xf4);
    for (;;) pause();  /* PID 1 must stay alive until QEMU handles the exit. */
}
static void fail(const char *msg) {
    dprintf(STDOUT_FILENO, "FAIL %s errno=%d\n", msg, errno);
    finish(1);
}
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static size_t load(const char *path, uint8_t *dst, size_t limit) {
    FILE *f = fopen(path,"rb");
    if (!f) fail(path);
    size_t n = fread(dst,1,limit,f);
    if (!n || ferror(f)) fail("reading corpus");
    fclose(f); return n;
}
static void report(struct store *s, const char *label) {
    uint64_t v[5];
    store_stats(s,v);
    uint64_t modeled, restored;
    store_selector_stats(s, &modeled, &restored);
    uint64_t programs, bytes, probes, nodes, refits, skips;
    store_search_stats(s,&programs,&bytes,&probes,&nodes);
    store_feedback_stats(s,&refits,&skips);
    printf("optimizer %s programs=%lu program_bytes=%lu probes=%lu nodes=%lu refits=%lu skips=%lu\n",
           label,programs,bytes,probes,nodes,refits,skips);
    printf("selector %s pages=%lu restores=%lu\n", label, modeled, restored);
    printf("store %s fill=%lu packed=%lu raw=%lu sparse=%lu backing_and_directory_bytes=%lu\n",
           label,v[0],v[1],v[2],v[3],v[4]);
}
static void project(struct store *s, const uint8_t *oracle, size_t pages, const char *label) {
    int32_t w[SIDE]; int64_t got[SIDE], expected[SIDE]; int packed = 0;
    uint64_t kernel_ns=0,reference_ns=0;
    for (size_t page=0; page<pages; page++) {
        for (int r=0; r<SIDE; r++) w[r]=(int)((r*17+page)%31)-15;
        uint64_t t=now();
        int route=store_project(s,page,w,got);
        kernel_ns+=now()-t;
        if (route<0) fail("tensor operation");
        packed+=route;
        t=now();
        for (int c=0; c<SIDE; c++) {
            expected[c]=0;
            for (int r=0; r<SIDE; r++)
                expected[c]+=(int64_t)w[r]*oracle[page*PAGE+r*SIDE+c];
        }
        reference_ns+=now()-t;
        if (memcmp(expected,got,sizeof got)) fail("tensor/dense mismatch");
    }
    printf("projection %s checked_i64=%zu packed_kernel_pages=%d mixed_dispatch_ns=%lu dense_oracle_ns=%lu\n",
           label,pages*SIDE,packed,kernel_ns,reference_ns);
}
int main(void) {
    int tty=open("/dev/ttyS0",O_RDWR);
    if (tty<0 || ioperm(0xf4,1,1)) return 1;
    dup2(tty,1); dup2(tty,2); setbuf(stdout,NULL);
    if (mount("proc","/proc","proc",0,NULL) || mount("sysfs","/sys","sysfs",0,NULL)) fail("mount");
    size_t len=8u<<20, pages=len/PAGE;
    uint8_t *oracle=calloc(1,len);
    if (!oracle) fail("oracle");
    size_t digits=load("/digits",oracle,1u<<20);
    size_t kernel=load("/kernel-prefix",oracle+(1u<<20),4u<<20);
    struct store *s=store_new(pages);
    if (!s) fail("store");
    for (size_t i=0; i<pages; i++) if (store_put(s,i,oracle+i*PAGE)) fail("ingest");
    printf("corpus logical_bytes=%zu digits_bytes=%zu kernel_bytes=%zu\n",len,digits,kernel);
    report(s,"initial");
    uint8_t *mapped=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (mapped==MAP_FAILED) fail("mmap");
    struct pager *p=pager_attach_vm(mapped,len,s,true,true);
    if (!p) fail("attach");
    project(s,oracle,pages,"initial");
    if (pager_faults(p)) fail("cold contraction faulted managed pages");
    if (memcmp(mapped,oracle,len)) fail("initial fault-in");
    /* Distinct new bytes include writes straddling page boundaries. */
    for (size_t i=PAGE-3; i+8<=len; i+=17*PAGE) {
        uint64_t marker=0xd37f000000000000ull ^ i;
        memcpy(mapped+i,&marker,8);
        memcpy(oracle+i,&marker,8);
    }
    if (pager_checkpoint(p)) fail("checkpoint");
    unsigned char *res=calloc(pages,1);
    if (!res || mincore(mapped,len,res)) fail("residency");
    for (size_t i=0;i<pages;i++) if (res[i]&1) fail("resident after reclaim");
    uint64_t before=pager_faults(p);
    project(s,oracle,pages,"after_write");
    if (pager_faults(p)!=before) fail("encoded contraction caused page-in");
    if (memcmp(mapped,oracle,len)) fail("refault after checkpoint");
    if (before!=pages || pager_faults(p)!=2*pages) fail("unexpected fault count");
    report(s,"after_write");
    printf("pager checked_bytes_per_scan=%zu pages_reclaimed=%zu faults_after_two_scans=%lu\n",len,pages,pager_faults(p));
    /* Independent, high-entropy full overwrite tests the raw promotion path. */
    uint32_t seed=0x61727475;
    for (size_t i=0; i<len; i++) {
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        oracle[i]=mapped[i]=(uint8_t)seed;
    }
    if (pager_checkpoint(p)) fail("dense checkpoint");
    report(s,"full_overwrite");
    if (memcmp(mapped,oracle,len)) fail("full overwrite refault");
    uint64_t stats[5]; store_stats(s,stats);
    if (stats[2]!=pages) fail("high entropy pages expected raw");
    /* Remove groups from the middle of the resident list and then drain to a target.
     * The readback checks pages reached through both eviction paths. */
    struct reclaim_result dropped;
    if (pager_checkpoint_range(p, 64 * PAGE, 64 * PAGE, &dropped) || dropped.discarded != 64)
        fail("interior group reclaim");
    if (pager_trim(p, 19)) fail("indexed trim");
    struct pager_metrics metrics;
    pager_metrics(p, &metrics);
    if (metrics.resident != 19 || metrics.automatic_drops != pages - 64 - 19)
        fail("resident group bookkeeping");
    if (memcmp(mapped,oracle,len)) fail("mixed manual and automatic readback");
    printf("indexed_trim resident=%lu automatic_drops=%lu verified_bytes=%zu\n",
           metrics.resident, metrics.automatic_drops, len);
    pager_detach(p); munmap(mapped,len); store_free(s); free(res); free(oracle);
#ifdef TEST_SVM
    if (load_svm_modules()) fail("loading SVM modules");
    const char *accel = "kvm";
#else
    const char *accel = "tcg";
#endif
    pid_t child=fork();
    if (!child) {
        execl("/qemu","qemu","-machine","pc,memory-backend=mem,mem-merge=off","-accel",accel,
              "-object","memory-backend-verdant-tensor,id=mem,size=16M,image=/image","-m","16","-smp","1",
              "-nodefaults","-display","none","-monitor","none","-serial","none","-L","/firmware","-bios","/inner.rom",
              "-device","isa-debug-exit,iobase=0xf4,iosize=4","-no-reboot",(char *)NULL);
        _exit(127);
    }
    int status;
    if (child<0 || waitpid(child,&status,0)<0 || !WIFEXITED(status) || WEXITSTATUS(status)!=1)
        fail("nested QEMU main RAM");
    printf("PASS nested %s main RAM and Linux pager\n", !strcmp(accel,"kvm") ? "KVM" : "TCG");
    finish(0);
}
