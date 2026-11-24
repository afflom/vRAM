// SPDX-License-Identifier: GPL-2.0
/* Specialization of the Verdant store to inline Fill and exact literal pages. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include "page_io.h"

static unsigned long capacity_mib = 64;
module_param(capacity_mib, ulong, 0444);
MODULE_PARM_DESC(capacity_mib, "Logical block capacity in MiB");
static unsigned long slot_count;
struct slot {
	u8 *literal;
	spinlock_t lock;
	u8 fill;
};

static struct slot *directory;
static struct gendisk *disk;
static int major;
static atomic_long_t retained = ATOMIC_LONG_INIT(0);
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

static void transfer(struct slot *slot, struct bio *bio, struct bvec_iter page, bool read)
{
	struct bvec_iter iter;
	struct bio_vec vec;
	size_t offset = 0;

	__bio_for_each_segment(vec, bio, iter, page) {
		void *mapped = bvec_kmap_local(&vec);

		if (read) {
			if (slot->literal)
				memcpy(mapped, slot->literal + offset, vec.bv_len);
			else
				memset(mapped, slot->fill, vec.bv_len);
			flush_dcache_page(vec.bv_page);
		} else {
			memcpy(slot->literal + offset, mapped, vec.bv_len);
		}
		kunmap_local(mapped);
		offset += vec.bv_len;
	}
}

static bool transfer_page(struct bio *bio, struct bvec_iter page, unsigned int action)
{
	struct slot *slot;
	unsigned long flags;
	u8 value = 0;
	u8 *spare = NULL;
	bool fill;

	slot = &directory[page.bi_sector / SECTORS_PER_SLOT];
	fill = action == 2 && inline_fill[uniform_page(bio, page, &value)];
	spin_lock_irqsave(&slot->lock, flags);
	if (action == 1) {
		transfer(slot, bio, page, true);
	} else if (fill) {
		spare = slot->literal;
		slot->literal = NULL;
		slot->fill = value;
		if (spare)
			atomic_long_dec(&retained);
	} else {
		if (!slot->literal) {
			spin_unlock_irqrestore(&slot->lock, flags);
			spare = (u8 *)__get_free_page(GFP_NOIO | __GFP_NORETRY | __GFP_NOWARN);
			spin_lock_irqsave(&slot->lock, flags);
			if (!slot->literal) {
				if (!spare) {
					spin_unlock_irqrestore(&slot->lock, flags);
					return false;
				}
				slot->literal = spare;
				spare = NULL;
				atomic_long_inc(&retained);
			}
		}
		transfer(slot, bio, page, false);
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	if (spare)
		free_page((unsigned long)spare);
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
	struct slot *slot;
	unsigned long flags;
	u8 *previous;

	if (index >= slot_count)
		return;
	slot = &directory[index];
	spin_lock_irqsave(&slot->lock, flags);
	previous = slot->literal;
	slot->literal = NULL;
	slot->fill = 0;
	if (previous)
		atomic_long_dec(&retained);
	spin_unlock_irqrestore(&slot->lock, flags);
	if (previous)
		free_page((unsigned long)previous);
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
	unsigned long i;
	int rc;

	BUILD_BUG_ON(PAGE_SIZE != SLOT);
	if (!capacity_mib || capacity_mib > UINT_MAX / (SZ_1M / SLOT))
		return -EINVAL;
	slot_count = capacity_mib * (SZ_1M / SLOT);
	directory_bytes = PAGE_ALIGN(array_size(slot_count, sizeof(*directory)));
	directory = vzalloc(directory_bytes);
	if (!directory)
		return -ENOMEM;
	for (i = 0; i < slot_count; i++)
		spin_lock_init(&directory[i].lock);
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
		if (directory[i].literal)
			free_page((unsigned long)directory[i].literal);
	vfree(directory);
}

module_init(fill_init);
module_exit(fill_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Verdant resident Fill and literal swap device");
