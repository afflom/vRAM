// SPDX-License-Identifier: GPL-2.0
/* Resident Verdant store with inline fills, exact folded blocks and literals. */
#include <linux/bio.h>
#include <linux/bit_spinlock.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "page_io.h"

static unsigned long capacity_mib = 64;
module_param(capacity_mib, ulong, 0444);
MODULE_PARM_DESC(capacity_mib, "Logical block capacity in MiB");
static unsigned long slot_count;
/* A frame address has twelve zero low bits. An inline fill uses bits 0..7.
 * Bit 8 serializes this slot independently in every representation. */
static unsigned long *directory;

static unsigned long lock_slot(unsigned long *slot)
{
	unsigned long flags;

	local_irq_save(flags);
	bit_spin_lock(__ffs(VRAM_SLOT_LOCK), slot);
	return flags;
}

static void unlock_slot(unsigned long *slot, unsigned long flags)
{
	/* All payload writers hold this lock, permitting the release-store variant. */
	__bit_spin_unlock(__ffs(VRAM_SLOT_LOCK), slot);
	local_irq_restore(flags);
}

static void replace_payload(unsigned long *slot, unsigned long payload)
{
	/* UP kernels can elide the lock bit. Preserve its actual held state. */
	WRITE_ONCE(*slot, (READ_ONCE(*slot) & VRAM_SLOT_LOCK) | payload);
}

static struct gendisk *disk;
static int major;
static atomic_long_t retained = ATOMIC_LONG_INIT(0);
static atomic_long_t block_bytes = ATOMIC_LONG_INIT(0);
static unsigned long directory_bytes;
static char policy[] = VRAM_HOLO;
module_param_string(policy, policy, sizeof(policy), 0444);

static int get_directory(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lu\n", directory_bytes);
}

static int get_literals(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&retained));
}

static int get_blocks(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&block_bytes));
}

static const struct kernel_param_ops block_ops = { .get = get_blocks };
module_param_cb(repeated_bytes, &block_ops, NULL, 0444);
static const struct kernel_param_ops directory_ops = { .get = get_directory };
static const struct kernel_param_ops literal_ops = { .get = get_literals };
module_param_cb(directory_bytes, &directory_ops, NULL, 0444);
module_param_cb(literal_pages, &literal_ops, NULL, 0444);

static bool uniform_page(struct bio *bio, struct bvec_iter page, u8 *value)
{
	struct bvec_iter iter;
	struct bio_vec vec;
	bool first = true;

	__bio_for_each_segment(vec, bio, iter, page) {
		const u8 *mapped = bvec_kmap_local(&vec);
		bool matches;

		if (first) {
			*value = *mapped;
			first = false;
		}
		matches = !memchr_inv(mapped, *value, vec.bv_len);
		kunmap_local(mapped);
		if (!matches)
			return false;
	}
	return !first;
}

/* The class occupies bits 9..11. A power-of-two kmalloc of at least 32
 * bytes leaves five zero address bits, so its page offset fits in bits 0..6.
 * The directory lock at bit 8 is independent of both fields. */
#define MIN_ORDER 5
#define PAGE_ORDER 12
#define FOLD_SHIFT 9

static unsigned int payload_size(unsigned long word)
{
	unsigned int tag = offset_in_page(word) >> FOLD_SHIFT;

	if (!(word & PAGE_MASK))
		return 0;
	return tag ? 1U << (MIN_ORDER - 1 + tag) : SLOT;
}

static u8 *payload_address(unsigned long word)
{
	unsigned long base = word & PAGE_MASK;
	unsigned long offset = word & ((SLOT >> MIN_ORDER) - 1);

	return (u8 *)(base | (offset << MIN_ORDER));
}

static unsigned long allocate_payload(unsigned int size)
{
	gfp_t flags = GFP_NOIO | __GFP_NORETRY | __GFP_NOWARN;
	unsigned long addr;
	unsigned int tag;

	if (size == SLOT)
		return __get_free_page(flags);
	addr = (unsigned long)kmalloc(size, flags);
	if (!addr)
		return 0;
	tag = ilog2(size) - MIN_ORDER + 1;
	return (addr & PAGE_MASK) | (offset_in_page(addr) >> MIN_ORDER) |
	       (tag << FOLD_SHIFT);
}

static void free_payload(unsigned long word)
{
	unsigned int size = payload_size(word);

	if (size == SLOT)
		free_page(word & PAGE_MASK);
	else if (size)
		kfree(payload_address(word));
}

static void account_payload(unsigned int size, int delta)
{
	if (size == SLOT)
		atomic_long_add(delta, &retained);
	else if (size)
		atomic_long_add((long)size * delta, &block_bytes);
}

/* bio segments may split either half at any byte. Compare the original
 * source spans without allocating a temporary page under memory pressure. */
static bool split_halves_equal(struct bio *bio, struct bvec_iter left, unsigned int half)
{
	struct bvec_iter right = left;
	unsigned int remaining = half;

	bio_advance_iter(bio, &right, half);
	while (remaining) {
		struct bio_vec a = bio_iter_iovec(bio, left);
		struct bio_vec b = bio_iter_iovec(bio, right);
		unsigned int n = min3(remaining, a.bv_len, b.bv_len);
		void *x = bvec_kmap_local(&a);
		void *y = bvec_kmap_local(&b);
		bool equal = !memcmp(x, y, n);

		kunmap_local(y);
		kunmap_local(x);
		if (!equal)
			return false;
		bio_advance_iter(bio, &left, n);
		bio_advance_iter(bio, &right, n);
		remaining -= n;
	}
	return true;
}

static unsigned int folded_size(struct bio *bio, struct bvec_iter page)
{
	struct bio_vec vec = bio_iter_iovec(bio, page);
	const u8 *mapped = vec.bv_len == SLOT ? bvec_kmap_local(&vec) : NULL;
	unsigned int order = PAGE_ORDER;

	while (order > MIN_ORDER) {
		unsigned int half = 1U << (order - 1);
		bool equal = mapped ? !memcmp(mapped, mapped + half, half) :
				      split_halves_equal(bio, page, half);
		unsigned int next = fold_order[order - MIN_ORDER][equal];

		if (next == order)
			break;
		order = next;
	}
	if (mapped)
		kunmap_local(mapped);
	return 1U << order;
}

static void repeat_into(u8 *out, unsigned int count, const u8 *block,
			unsigned int size, unsigned int offset)
{
	unsigned int phase = offset & (size - 1);
	unsigned int done = min(count, size - phase);
	unsigned int wrap = min(count - done, phase);

	memcpy(out, block + phase, done);
	memcpy(out + done, block, wrap);
	done += wrap;
	while (done < count) {
		unsigned int n = min(done, count - done);

		memcpy(out + done, out, n);
		done += n;
	}
}

static void transfer(unsigned long word, unsigned int size, struct bio *bio,
		     struct bvec_iter page, bool read)
{
	struct bvec_iter iter;
	struct bio_vec vec;
	u8 *data = payload_address(word);
	unsigned int offset = 0;

	if (!read)
		page.bi_size = size;
	__bio_for_each_segment(vec, bio, iter, page) {
		void *mapped = bvec_kmap_local(&vec);

		if (read) {
			if (size == SLOT)
				memcpy(mapped, data + offset, vec.bv_len);
			else if (size)
				repeat_into(mapped, vec.bv_len, data, size, offset);
			else
				memset(mapped, (u8)word, vec.bv_len);
			flush_dcache_page(vec.bv_page);
		} else {
			memcpy(data + offset, mapped, vec.bv_len);
		}
		kunmap_local(mapped);
		offset += vec.bv_len;
	}
}

static bool transfer_page(struct bio *bio, struct bvec_iter page, unsigned int action)
{
	unsigned long *slot = &directory[page.bi_sector / SECTORS_PER_SLOT];
	unsigned long flags, old, spare = 0;
	unsigned int size = 0, previous;
	u8 value = 0;

	if (action == 2 && !inline_fill[uniform_page(bio, page, &value)])
		size = folded_size(bio, page);

	/* A speculative class check avoids taking the slot lock across the
	 * allocation decision. Only dereference a payload after rechecking it
	 * under the lock, even when another writer installs the same class. */
	if (size && payload_size(READ_ONCE(*slot)) == size) {
		flags = lock_slot(slot);
		old = READ_ONCE(*slot);
		if (payload_size(old) == size) {
			transfer(old, size, bio, page, false);
			unlock_slot(slot, flags);
			return true;
		}
		unlock_slot(slot, flags);
	}
	if (size)
		spare = allocate_payload(size);
	flags = lock_slot(slot);
	old = READ_ONCE(*slot);
	previous = payload_size(old);
	if (action == 1) {
		transfer(old, previous, bio, page, true);
		unlock_slot(slot, flags);
		return true;
	}
	if (size && !spare && size != previous) {
		/* An existing literal can take any write under allocation pressure. */
		if (previous != SLOT) {
			unlock_slot(slot, flags);
			return false;
		}
		size = SLOT;
	}
	if (size != previous) {
		unsigned long replacement = size ? spare : value;

		replace_payload(slot, replacement);
		account_payload(previous, -1);
		account_payload(size, 1);
		spare = old;
		old = replacement;
	} else if (!size) {
		replace_payload(slot, value);
	}
	if (size)
		transfer(old, size, bio, page, false);
	unlock_slot(slot, flags);
	free_payload(spare);
	return true;
}

static void fill_submit(struct bio *bio)
{
	struct bvec_iter cursor = bio->bi_iter;
	sector_t capacity = (sector_t)slot_count * SECTORS_PER_SLOT;
	unsigned int op = bio_op(bio) == REQ_OP_READ ? 0 : bio_op(bio) == REQ_OP_WRITE ? 1 : 2;
	bool inside = cursor.bi_sector <= capacity &&
		bio_sectors(bio) <= capacity - cursor.bi_sector;
	bool whole = cursor.bi_size && !(cursor.bi_size % SLOT);
	unsigned int shape = (!(cursor.bi_sector % SECTORS_PER_SLOT) << 2) | (whole << 1) | inside;
	unsigned int action = resident_action[op * 8 + shape];

	if (!action) {
		bio_io_error(bio);
		return;
	}
	while (cursor.bi_size) {
		struct bvec_iter page = cursor;

		page.bi_size = SLOT;
		if (!transfer_page(bio, page, action)) {
			bio->bi_status = BLK_STS_RESOURCE;
			break;
		}
		if (cursor.bi_size == SLOT)
			break;
		bio_advance_iter(bio, &cursor, SLOT);
	}
	bio_endio(bio);
}

static void release_swap_slot(struct block_device *bdev, unsigned long index)
{
	unsigned long *slot;
	unsigned long flags;
	unsigned long previous;

	if (index >= slot_count)
		return;
	slot = &directory[index];
	flags = lock_slot(slot);
	previous = READ_ONCE(*slot);
	replace_payload(slot, 0);
	account_payload(payload_size(previous), -1);
	unlock_slot(slot, flags);
	free_payload(previous);
}

static const struct block_device_operations fill_ops = {
	.owner = THIS_MODULE,
	.submit_bio = fill_submit,
	.swap_slot_free_notify = release_swap_slot,
};

static int __init fill_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct queue_limits limits = {
		.logical_block_size = SLOT,
		.physical_block_size = SLOT,
		.max_hw_sectors = 512,
	};
#endif
	int rc;

	BUILD_BUG_ON(PAGE_SIZE != SLOT || sizeof(*directory) != VRAM_SLOT_BYTES);
	BUILD_BUG_ON(VRAM_SLOT_LOCK != BIT(FOLD_SHIFT - 1) || SLOT != BIT(PAGE_ORDER));
	if (!capacity_mib || capacity_mib > UINT_MAX / (SZ_1M / SLOT))
		return -EINVAL;
	slot_count = capacity_mib * (SZ_1M / SLOT);
	directory_bytes = PAGE_ALIGN(array_size(slot_count, sizeof(*directory)));
	directory = vzalloc(directory_bytes);
	if (!directory)
		return -ENOMEM;
	major = register_blkdev(0, "vramfill");
	if (major < 0) {
		rc = major;
		goto free_directory;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		rc = PTR_ERR(disk);
		goto unregister;
	}
#else
	disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!disk) {
		rc = -ENOMEM;
		goto unregister;
	}
	blk_queue_logical_block_size(disk->queue, SLOT);
	blk_queue_physical_block_size(disk->queue, SLOT);
	blk_queue_max_hw_sectors(disk->queue, 512);
#endif
	disk->major = major;
	disk->minors = 1;
	disk->flags |= GENHD_FL_NO_PART;
	disk->fops = &fill_ops;
	strscpy(disk->disk_name, "vramfill0", DISK_NAME_LEN);
	set_capacity(disk, (sector_t)slot_count * SECTORS_PER_SLOT);
	rc = add_disk(disk);
	if (!rc)
		return 0;
	put_disk(disk);
unregister:
	unregister_blkdev(major, "vramfill");
free_directory:
	vfree(directory);
	return rc;
}

static void __exit fill_exit(void)
{
	unsigned long i;

	del_gendisk(disk);
	put_disk(disk);
	unregister_blkdev(major, "vramfill");
	for (i = 0; i < slot_count; i++)
		free_payload(directory[i]);
	vfree(directory);
}

module_init(fill_init);
module_exit(fill_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Verdant resident folded-page swap device");
