#ifndef VERDANT_PAGER_H
#define VERDANT_PAGER_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
struct store;
struct pager;
uint64_t vram_capacity_ceiling(uint64_t platform_mib, uint64_t guest_mib, uint64_t backing_bytes);
struct store *store_new(size_t npages);
/* fd is a private, preallocated, writable file of exactly (npages + 1) pages. */
struct store *store_new_file(size_t npages, int fd);
/* Distinct preallocated regular files together hold npages plus one replacement page. */
struct store *store_new_files(size_t npages, const int *fds, size_t count);
/* Successful exact backing operations. One range read can cover several slots. */
void store_file_stats(struct store *, uint64_t *reads, uint64_t *writes);
struct store *store_new_with_search(size_t npages, size_t node_budget);
struct store *store_new_config(size_t npages, size_t node_budget, bool adaptive);
void store_feedback_stats(struct store *, uint64_t *refits, uint64_t *skips);
void store_search_stats(struct store *, uint64_t *pages, uint64_t *bytes, uint64_t *searches, uint64_t *nodes);
void store_free(struct store *);
int store_put(struct store *, size_t page, const uint8_t *bytes4096);
int store_patch(struct store *, size_t page, size_t offset, const uint8_t *bytes, size_t len);
int store_write_pages(struct store *, size_t first, size_t count, const uint8_t *data);
int store_read_pages(struct store *, size_t first, size_t count, uint8_t *out);
/* Encoding-only reuse count, fit wall ns, overlapping commit ns, snapshot ns, validation ns, live credit replays, reacquisition ns. */
void store_fit_profile(struct store *, uint64_t metrics[7]);
uint64_t store_reacquire_commit_ns(struct store *);
void store_fit_counts(struct store *, uint64_t counts[2]);
void store_io_times(struct store *, uint64_t nanoseconds[4]);
int store_discard(struct store *, size_t start, size_t count);
uint64_t vram_host_auto_mib(uint64_t available_mib);
int store_get(struct store *, size_t page, uint8_t *bytes4096);
int store_project_rows(struct store *, size_t page, size_t rows,
                       const int32_t weights[][64], int64_t output[][64]);
int store_project(struct store *, size_t page, const int32_t *weights64, int64_t *output64);
void store_selector_stats(struct store *, uint64_t *pages, uint64_t *restores);
void store_stats(struct store *, uint64_t counters[5]);

/* Caller owns the private, anonymous, initially untouched 4 KiB-page mapping
 * and the store. Quiesce all access before checkpoint or detach. For direct
 * queries checkpoint first, then keep the covered pages quiescent. */
enum vram_action { VRAM_SKIP, VRAM_RESTORE_CLEAN, VRAM_RESTORE_DIRTY, VRAM_WRITABLE,
                   VRAM_CAPTURE, VRAM_DISCARD, VRAM_RELEASE, VRAM_RETAIN, VRAM_TRIM, VRAM_ALLOW };
uint32_t vram_fault(bool wp, bool write, bool tracking);
uint32_t vram_reclaim(bool present, bool dirty);
uint32_t vram_captured(bool success);
uint32_t vram_discarded(bool success);
bool vram_trim(uint64_t limit, uint64_t resident);
bool vram_permitted(bool opted, bool quiescent, bool tcg, bool kvm, bool capture_all, bool migration_idle);
uint64_t vram_membership(uint64_t word, uint64_t bit, bool present);
/* BlockPolicy effect tags on the C ABI. */
enum vram_block_effect {
    VRAM_BLOCK_HEADER, VRAM_BLOCK_READ_HEAD, VRAM_BLOCK_READ_TAIL, VRAM_BLOCK_WRITE,
    VRAM_BLOCK_TRIM, VRAM_BLOCK_DRAIN, VRAM_BLOCK_ACK, VRAM_BLOCK_INVALID,
    VRAM_BLOCK_UNSUPPORTED, VRAM_BLOCK_READ_ERROR, VRAM_BLOCK_DONE,
    VRAM_BLOCK_DISCONNECT, VRAM_BLOCK_ABORT, VRAM_BLOCK_SEND_HEAD, VRAM_BLOCK_SEND_TAIL
};
uint32_t vram_block_begin(bool framed, uint16_t command, uint64_t offset, uint32_t length, uint64_t extent);
uint32_t vram_block_after(uint32_t effect, bool success, bool last);
bool vram_range(uint64_t offset, uint64_t length, uint64_t extent);
void store_literal_stats(struct store *, uint64_t *unique, uint64_t *hits, uint64_t *skipped, uint64_t *residuals);
void store_edits(struct store *, uint64_t *pages, uint64_t *entries);
struct pager_metrics {
    uint64_t resident, clean_drops, captures, writes, automatic_drops;
    bool write_tracking;
};
void pager_metrics(struct pager *, struct pager_metrics *);
bool pager_over_target(struct pager *, uint64_t target_pages);
int pager_trim(struct pager *, uint64_t target_pages);
struct pager *pager_attach(void *base, size_t bytes, struct store *store);
struct pager *pager_attach_vm(void *base, size_t bytes, struct store *store, bool track_writes,
                              bool index_residents);
bool pager_tracks_writes(struct pager *);
struct reclaim_result {
    uint64_t discarded;
    uint64_t resident_after;
};
int pager_checkpoint_range(struct pager *, uint64_t offset, uint64_t bytes,
                           struct reclaim_result *);
int pager_checkpoint(struct pager *);
uint64_t pager_faults(struct pager *);
void pager_detach(struct pager *);
#endif
