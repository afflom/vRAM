/* SPDX-License-Identifier: GPL-2.0 */
#include "literal_policy.h"

#define SLOT SZ_4K
#define SECTORS_PER_SLOT (SLOT >> SECTOR_SHIFT)
#define CAPACITY_SECTORS (SZ_64M >> SECTOR_SHIFT)

static inline unsigned int page_action(const struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int op, shape;

	switch (bio_op(bio)) {
	case REQ_OP_READ: op = 0; break;
	case REQ_OP_WRITE: op = 1; break;
	default: op = 2; break;
	}
	shape = (!(sector % SECTORS_PER_SLOT) << 2) |
		((bio->bi_iter.bi_size == SLOT) << 1) |
		(sector <= CAPACITY_SECTORS - SECTORS_PER_SLOT);
	return literal_action[op * 8 + shape];
}
