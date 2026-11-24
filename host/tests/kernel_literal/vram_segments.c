// SPDX-License-Identifier: GPL-2.0
/* Loaded only by the disposable guest. Exercise sub-page bio boundaries that
 * the aligned userspace benchmark cannot select. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/module.h>

static int scatter(struct block_device *dev, blk_opf_t op, u8 *page,
		   const unsigned int lengths[3])
{
	struct page *parts[3] = {};
	struct bio *bio = bio_alloc(dev, 3, op, GFP_KERNEL);
	unsigned int offset = 0, i;
	int err = -ENOMEM;

	if (!bio)
		return err;
	bio->bi_iter.bi_sector = 700 * (4096 / 512);
	for (i = 0; i < 3; i++) {
		parts[i] = alloc_page(GFP_KERNEL);
		if (!parts[i])
			goto out;
		if (op == REQ_OP_WRITE)
			memcpy_to_page(parts[i], 13, page + offset, lengths[i]);
		if (bio_add_page(bio, parts[i], lengths[i], 13) != lengths[i]) {
			err = -EIO;
			goto out;
		}
		offset += lengths[i];
	}
	err = submit_bio_wait(bio);
	if (!err && op == REQ_OP_READ) {
		offset = 0;
		for (i = 0; i < 3; i++) {
			memcpy_from_page(page + offset, parts[i], 13, lengths[i]);
			offset += lengths[i];
		}
	}
out:
	bio_put(bio);
	for (i = 0; i < 3; i++)
		if (parts[i])
			__free_page(parts[i]);
	return err;
}

static int __init check_segments(void)
{
	static const unsigned int writes[] = {37, 1987, 2072};
	static const unsigned int reads[] = {1111, 2049, 936};
	struct file *file;
	u8 *data, *output;
	int err = -ENOMEM;
	unsigned int i, trial;

	file = bdev_file_open_by_path("/dev/vramfill0", BLK_OPEN_READ | BLK_OPEN_WRITE,
				      THIS_MODULE, NULL);
	if (IS_ERR(file))
		return PTR_ERR(file);
	data = kmalloc(4096, GFP_KERNEL);
	output = kmalloc(4096, GFP_KERNEL);
	if (!data || !output)
		goto out;
	for (i = 0; i < 4096; i++)
		data[i] = i;
	for (trial = 0; trial < 3; trial++) {
		if (trial)
			data[4095] ^= 1;
		err = scatter(file_bdev(file), REQ_OP_WRITE, data, writes);
		if (err)
			break;
		err = scatter(file_bdev(file), REQ_OP_READ, output, reads);
		if (err)
			break;
		if (memcmp(data, output, 4096)) {
			err = -EIO;
			break;
		}
	}
out:
	kfree(output);
	kfree(data);
	fput(file);
	return err;
}

static void __exit done(void) {}
module_init(check_segments);
module_exit(done);
MODULE_LICENSE("GPL");
