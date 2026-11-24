// SPDX-License-Identifier: GPL-2.0
/* Disposable latency control. Every address has its own resident literal slot. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include "page_io.h"

static u8 *slots;
static spinlock_t *slot_locks;
static struct gendisk *disk;
static int major;
static char policy[] = VRAM_HOLO;
module_param_string(policy, policy, sizeof(policy), 0444);

static void literal_submit(struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int action;
	struct bvec_iter iter;
	struct bio_vec vec;
	unsigned long flags;
	size_t index, offset;

	action = page_action(bio);
	if (!action) {
		bio_io_error(bio);
		return;
	}
	index = sector / SECTORS_PER_SLOT;
	offset = index * SLOT;
	spin_lock_irqsave(&slot_locks[index], flags);
	bio_for_each_segment(vec, bio, iter) {
		void *mapped = bvec_kmap_local(&vec);

		if (action == 1) {
			memcpy(mapped, slots + offset, vec.bv_len);
			flush_dcache_page(vec.bv_page);
		} else {
			memcpy(slots + offset, mapped, vec.bv_len);
		}
		kunmap_local(mapped);
		offset += vec.bv_len;
	}
	spin_unlock_irqrestore(&slot_locks[index], flags);
	bio_endio(bio);
}

static const struct block_device_operations literal_ops = {
	.owner = THIS_MODULE,
	.submit_bio = literal_submit,
};

static int __init literal_init(void)
{
	struct queue_limits limits = {
		.logical_block_size = SLOT,
		.physical_block_size = SLOT,
		.max_hw_sectors = SECTORS_PER_SLOT,
	};
	int rc, i;

	slots = vzalloc(SZ_64M);
	slot_locks = kvmalloc_array(SZ_64M / SLOT, sizeof(*slot_locks), GFP_KERNEL);
	if (!slots || !slot_locks) {
		rc = -ENOMEM;
		goto free_store;
	}
	for (i = 0; i < SZ_64M / SLOT; i++)
		spin_lock_init(&slot_locks[i]);
	major = register_blkdev(0, "vramlit");
	if (major < 0) {
		rc = major;
		goto free_store;
	}
	disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		rc = PTR_ERR(disk);
		goto unregister;
	}
	disk->major = major;
	disk->minors = 1;
	disk->flags |= GENHD_FL_NO_PART;
	disk->fops = &literal_ops;
	strscpy(disk->disk_name, "vramlit0", DISK_NAME_LEN);
	set_capacity(disk, CAPACITY_SECTORS);
	rc = add_disk(disk);
	if (!rc)
		return 0;
	put_disk(disk);
unregister:
	unregister_blkdev(major, "vramlit");
free_store:
	kvfree(slot_locks);
	vfree(slots);
	return rc;
}

static void __exit literal_exit(void)
{
	del_gendisk(disk);
	put_disk(disk);
	unregister_blkdev(major, "vramlit");
	kvfree(slot_locks);
	vfree(slots);
}

module_init(literal_init);
module_exit(literal_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vRAM resident literal latency control for a disposable guest");
