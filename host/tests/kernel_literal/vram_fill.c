// SPDX-License-Identifier: GPL-2.0
/* Specialization of the Verdant store to inline Fill and exact literal pages. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "page_io.h"

#define SLOT_COUNT (SZ_64M / SLOT)
struct slot {
	u8 *literal;
	spinlock_t lock;
	u8 fill;
};

static struct slot *directory;
static struct gendisk *disk;
static int major;
static atomic_long_t retained = ATOMIC_LONG_INIT(0);
static unsigned long directory_bytes = PAGE_ALIGN(SLOT_COUNT * sizeof(struct slot));
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

static bool uniform_bio(struct bio *bio, u8 *value)
{
	struct bvec_iter iter;
	struct bio_vec vec;
	bool first = true;

	bio_for_each_segment(vec, bio, iter) {
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

static void transfer(struct slot *slot, struct bio *bio, bool read)
{
	struct bvec_iter iter;
	struct bio_vec vec;
	size_t offset = 0;

	bio_for_each_segment(vec, bio, iter) {
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

static void fill_submit(struct bio *bio)
{
	unsigned int action = page_action(bio);
	struct slot *slot;
	unsigned long flags;
	u8 value = 0;
	u8 *spare = NULL;
	bool fill;

	if (!action) {
		bio_io_error(bio);
		return;
	}
	slot = &directory[bio->bi_iter.bi_sector / SECTORS_PER_SLOT];
	fill = action == 2 && inline_fill[uniform_bio(bio, &value)];
	spin_lock_irqsave(&slot->lock, flags);
	if (action == 1) {
		transfer(slot, bio, true);
	} else if (fill) {
		spare = slot->literal;
		slot->literal = NULL;
		slot->fill = value;
		if (spare)
			atomic_long_dec(&retained);
	} else {
		if (!slot->literal) {
			spin_unlock_irqrestore(&slot->lock, flags);
			spare = (u8 *)__get_free_page(GFP_NOIO);
			spin_lock_irqsave(&slot->lock, flags);
			if (!slot->literal) {
				if (!spare) {
					spin_unlock_irqrestore(&slot->lock, flags);
					bio->bi_status = BLK_STS_RESOURCE;
					bio_endio(bio);
					return;
				}
				slot->literal = spare;
				spare = NULL;
				atomic_long_inc(&retained);
			}
		}
		transfer(slot, bio, false);
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	if (spare)
		free_page((unsigned long)spare);
	bio_endio(bio);
}

static const struct block_device_operations fill_ops = {
	.owner = THIS_MODULE,
	.submit_bio = fill_submit,
};

static int __init fill_init(void)
{
	struct queue_limits limits = {
		.logical_block_size = SLOT,
		.physical_block_size = SLOT,
		.max_hw_sectors = SECTORS_PER_SLOT,
	};
	int rc, i;

	BUILD_BUG_ON(PAGE_SIZE != SLOT);
	directory = alloc_pages_exact(directory_bytes, GFP_KERNEL | __GFP_ZERO);
	if (!directory)
		return -ENOMEM;
	for (i = 0; i < SLOT_COUNT; i++)
		spin_lock_init(&directory[i].lock);
	major = register_blkdev(0, "vramfill");
	if (major < 0) {
		rc = major;
		goto free_directory;
	}
	disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		rc = PTR_ERR(disk);
		goto unregister;
	}
	disk->major = major;
	disk->minors = 1;
	disk->flags |= GENHD_FL_NO_PART;
	disk->fops = &fill_ops;
	strscpy(disk->disk_name, "vramfill0", DISK_NAME_LEN);
	set_capacity(disk, CAPACITY_SECTORS);
	rc = add_disk(disk);
	if (!rc)
		return 0;
	put_disk(disk);
unregister:
	unregister_blkdev(major, "vramfill");
free_directory:
	free_pages_exact(directory, directory_bytes);
	return rc;
}

static void __exit fill_exit(void)
{
	int i;

	del_gendisk(disk);
	put_disk(disk);
	unregister_blkdev(major, "vramfill");
	for (i = 0; i < SLOT_COUNT; i++)
		if (directory[i].literal)
			free_page((unsigned long)directory[i].literal);
	free_pages_exact(directory, directory_bytes);
}

module_init(fill_init);
module_exit(fill_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Verdant Fill and literal page store for the disposable latency guest");
