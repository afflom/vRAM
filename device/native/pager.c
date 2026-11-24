#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pager.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { PAGE = 4096 };
enum frame_state { ABSENT, CLEAN, DIRTY };
/* Nonempty 64-frame groups, ordered by when the group first became resident. */
struct frame_group {
    uint64_t mask;
    size_t prev, next;
};
struct pager {
    uint8_t *base;
    size_t len;
    struct store *store;
    int fd, shutdown;
    pthread_t worker;
    _Atomic unsigned char *populated;
    atomic_uint_fast64_t faults, resident, writes;
    uint64_t clean_drops, captures, automatic_drops;
    struct frame_group *groups;
    size_t oldest, newest;
    pthread_mutex_t index_lock;
    bool tracking;
};
static void fatal_fault(void) {
    perror("unrecoverable Verdant page fault");
    abort(); /* A fabricated zero page would silently corrupt guest memory. */
}
static void index_frame(struct pager *p, size_t i, bool present) {
    if (!p->groups) return;
    size_t id = i / 64 + 1;
    pthread_mutex_lock(&p->index_lock);
    struct frame_group *g = &p->groups[id];
    uint64_t old = g->mask;
    g->mask = vram_membership(old, UINT64_C(1) << (i % 64), present);
    if (!old && g->mask) {
        g->prev = p->newest;
        g->next = 0;
        if (p->newest) p->groups[p->newest].next = id;
        else p->oldest = id;
        p->newest = id;
    } else if (old && !g->mask) {
        if (g->prev) p->groups[g->prev].next = g->next;
        else p->oldest = g->next;
        if (g->next) p->groups[g->next].prev = g->prev;
        else p->newest = g->prev;
    }
    pthread_mutex_unlock(&p->index_lock);
}
static bool next_frame(struct pager *p, size_t *frame) {
    pthread_mutex_lock(&p->index_lock);
    size_t id = p->oldest;
    if (id) *frame = (id - 1) * 64 + __builtin_ctzll(p->groups[id].mask);
    pthread_mutex_unlock(&p->index_lock);
    return id != 0;
}
static void *serve(void *arg) {
    struct pager *p = arg;
    struct pollfd fds[] = {{p->shutdown, POLLIN, 0}, {p->fd, POLLIN, 0}};
    _Alignas(PAGE) uint8_t page[PAGE];
    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            fatal_fault();
        }
        if (fds[0].revents & POLLIN) return NULL;
        struct uffd_msg msg;
        ssize_t n = read(p->fd, &msg, sizeof msg);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n != sizeof msg || msg.event != UFFD_EVENT_PAGEFAULT) fatal_fault();
        uintptr_t address = msg.arg.pagefault.address & ~(uintptr_t)(PAGE - 1);
        size_t offset = address - (uintptr_t)p->base;
        if (offset >= p->len) fatal_fault();
        size_t index = offset / PAGE;
        uint64_t flags = msg.arg.pagefault.flags;
        uint32_t action = vram_fault(flags & UFFD_PAGEFAULT_FLAG_WP,
                                     flags & UFFD_PAGEFAULT_FLAG_WRITE, p->tracking);
        if (action == VRAM_WRITABLE) {
            struct uffdio_writeprotect wp = {.range = {.start = address, .len = PAGE}};
            unsigned char expected = CLEAN;
            atomic_compare_exchange_strong(&p->populated[index], &expected, DIRTY);
            if (ioctl(p->fd, UFFDIO_WRITEPROTECT, &wp) < 0) fatal_fault();
            atomic_fetch_add(&p->writes, 1);
            continue;
        }
        if (store_get(p->store, index, page)) fatal_fault();
        struct uffdio_copy copy = {.src = (uintptr_t)page, .dst = address,
                                   .len = PAGE, .mode = UFFDIO_COPY_MODE_DONTWAKE};
        if (action == VRAM_RESTORE_CLEAN) copy.mode |= UFFDIO_COPY_MODE_WP;
        else if (action != VRAM_RESTORE_DIRTY) fatal_fault();
        if (ioctl(p->fd, UFFDIO_COPY, &copy) < 0) {
            if (errno == EEXIST) continue;
            fatal_fault();
        }
        atomic_store(&p->populated[index], action == VRAM_RESTORE_CLEAN ? CLEAN : DIRTY);
        index_frame(p, index, true);
        atomic_fetch_add(&p->resident, 1);
        atomic_fetch_add(&p->faults, 1);
        struct uffdio_range filled = {.start = address, .len = PAGE};
        if (ioctl(p->fd, UFFDIO_WAKE, &filled) < 0) fatal_fault();
    }
}
struct pager *pager_attach_vm(void *base, size_t len, struct store *store, bool track_writes,
                              bool index_residents) {
    if (!store || !len || len % PAGE || (uintptr_t)base % PAGE || sysconf(_SC_PAGESIZE) != PAGE) {
        errno = EINVAL; return NULL;
    }
    struct pager *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->base = base; p->len = len; p->store = store; p->fd = p->shutdown = -1;
    int err = pthread_mutex_init(&p->index_lock, NULL);
    if (err) { free(p); errno = err; return NULL; }
    if (index_residents) {
        p->groups = calloc(1 + (len / PAGE + 63) / 64, sizeof *p->groups);
        if (!p->groups) goto fail;
    }
    p->populated = calloc(len / PAGE, sizeof *p->populated);
    if (!p->populated || madvise(base, len, MADV_NOHUGEPAGE)) goto fail;
    p->shutdown = eventfd(0, EFD_CLOEXEC);
    if (p->shutdown < 0) goto fail;
    p->fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (p->fd < 0) goto fail;
    struct uffdio_api api = {.api = UFFD_API, .features = track_writes ? UFFD_FEATURE_PAGEFAULT_FLAG_WP : 0};
    if (ioctl(p->fd, UFFDIO_API, &api) == 0) {
        p->tracking = track_writes && (api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP);
    } else {
        if (errno != EINVAL) goto fail;
        api = (struct uffdio_api){.api = UFFD_API};
        if (ioctl(p->fd, UFFDIO_API, &api)) goto fail;
    }
    struct uffdio_register reg = {.range = {(uintptr_t)base, len}, .mode = UFFDIO_REGISTER_MODE_MISSING};
    if (p->tracking) reg.mode |= UFFDIO_REGISTER_MODE_WP;
    if (ioctl(p->fd, UFFDIO_REGISTER, &reg)) goto fail;
    p->tracking = p->tracking && (reg.ioctls & (1ULL << _UFFDIO_WRITEPROTECT));
    err = pthread_create(&p->worker, NULL, serve, p);
    if (err) { errno = err; goto fail; }
    return p;
fail: {
        int cause = errno;
        if (p->fd >= 0) close(p->fd);
        if (p->shutdown >= 0) close(p->shutdown);
        pthread_mutex_destroy(&p->index_lock); free(p->groups);
        free(p->populated); free(p); errno = cause; return NULL;
    }
}
struct pager *pager_attach(void *base, size_t len, struct store *store) {
    return pager_attach_vm(base, len, store, true, false);
}
bool pager_tracks_writes(struct pager *p) { return p->tracking; }
/* The owner has quiesced CPU and external writers before this call. */
static int reclaim_frame(struct pager *p, size_t i) {
    unsigned char state = atomic_load(&p->populated[i]);
    uint32_t action = vram_reclaim(state != ABSENT, state == DIRTY);
    if (action == VRAM_SKIP) return 0;
    uint8_t *page = p->base + i * PAGE;
    if (action == VRAM_CAPTURE) {
        action = vram_captured(store_put(p->store, i, page) == 0);
        if (action != VRAM_DISCARD) { errno = EIO; return -1; }
        p->captures++;
    }
    if (action != VRAM_DISCARD) { errno = EIO; return -1; }
    if (vram_discarded(madvise(page, PAGE, MADV_DONTNEED) == 0) != VRAM_RELEASE) return -1;
    atomic_store(&p->populated[i], ABSENT);
    index_frame(p, i, false);
    atomic_fetch_sub(&p->resident, 1);
    if (state == CLEAN) p->clean_drops++;
    return 1;
}
void pager_metrics(struct pager *p, struct pager_metrics *m) {
    *m = (struct pager_metrics){.resident=atomic_load(&p->resident), .clean_drops=p->clean_drops,
        .writes=atomic_load(&p->writes), .captures=p->captures, .automatic_drops=p->automatic_drops,
        .write_tracking=p->tracking};
}
bool pager_over_target(struct pager *p, uint64_t target) {
    return vram_trim(target, atomic_load(&p->resident));
}
int pager_trim(struct pager *p, uint64_t target) {
    if (!p->groups) { errno = EINVAL; return -1; }
    while (pager_over_target(p, target)) {
        size_t victim;
        if (!next_frame(p, &victim)) { errno = EIO; return -1; }
        int result = reclaim_frame(p, victim);
        if (result <= 0) {
            if (!result) errno = EIO;
            return -1;
        }
        p->automatic_drops++;
    }
    return 0;
}
int pager_checkpoint_range(struct pager *p, uint64_t offset, uint64_t bytes,
                           struct reclaim_result *result) {
    *result = (struct reclaim_result){0};
    if (!vram_range(offset, bytes, p->len)) {
        errno = EINVAL;
        return -1;
    }
    size_t first = offset / PAGE;
    size_t end = first + bytes / PAGE;
    for (size_t i = first; i < end; i++) {
        int discarded = reclaim_frame(p, i);
        if (discarded < 0) return -1;
        result->discarded += discarded;
    }
    unsigned char residency[256];
    for (size_t i = first; i < end; ) {
        size_t count = end - i;
        if (count > sizeof residency) count = sizeof residency;
        if (mincore(p->base + i * PAGE, count * PAGE, residency)) return -1;
        for (size_t j = 0; j < count; j++) result->resident_after += residency[j] & 1;
        i += count;
    }
    return 0;
}
int pager_checkpoint(struct pager *p) {
    struct reclaim_result result;
    return pager_checkpoint_range(p, 0, p->len, &result);
}
uint64_t pager_faults(struct pager *p) { return atomic_load(&p->faults); }
void pager_detach(struct pager *p) {
    if (!p) return;
    uint64_t signal = 1;
    if (write(p->shutdown, &signal, sizeof signal) != sizeof signal) fatal_fault();
    pthread_join(p->worker, NULL);
    close(p->fd); close(p->shutdown); pthread_mutex_destroy(&p->index_lock);
    free(p->groups); free(p->populated); free(p);
}
