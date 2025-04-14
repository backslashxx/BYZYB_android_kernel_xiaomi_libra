// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#include "internal.h"
#include <linux/mpage.h>

void erofs_unmap_metabuf(struct erofs_buf *buf)
{
	if (buf->kmap_type == EROFS_KMAP)
		kunmap(buf->page);
	else if (buf->kmap_type == EROFS_KMAP_ATOMIC)
		kunmap_atomic(buf->base);
	buf->base = NULL;
	buf->kmap_type = EROFS_NO_KMAP;
}

void erofs_put_metabuf(struct erofs_buf *buf)
{
	if (!buf->page)
		return;
	erofs_unmap_metabuf(buf);
	put_page(buf->page);
	buf->page = NULL;
}

/*
 * Derive the block size from inode->i_blkbits to make compatible with
 * anonymous inode in fscache mode.
 */
void *erofs_bread(struct erofs_buf *buf, erofs_blk_t blkaddr,
		  enum erofs_kmap_type type)
{
	struct inode *inode = buf->inode;
	erofs_off_t offset = (erofs_off_t)blkaddr << inode->i_blkbits;
	pgoff_t index = offset >> PAGE_SHIFT;
	struct page *page = buf->page;

	if (!page || page->index != index) {
		erofs_put_metabuf(buf);

		page = read_cache_page_gfp(inode->i_mapping, index,
				mapping_gfp_constraint(inode->i_mapping, ~__GFP_FS));
		if (IS_ERR(page))
			return page;

		/* should already be PageUptodate, no need to lock page */
		buf->page = page;
	}
	if (buf->kmap_type == EROFS_NO_KMAP) {
		if (type == EROFS_KMAP)
			buf->base = kmap(page);
		else if (type == EROFS_KMAP_ATOMIC)
			buf->base = kmap_atomic(page);
		buf->kmap_type = type;
	} else if (buf->kmap_type != type) {
		DBG_BUGON(1);
		return ERR_PTR(-EFAULT);
	}
	if (type == EROFS_NO_KMAP)
		return NULL;
	return buf->base + (offset & ~PAGE_MASK);
}

void erofs_init_metabuf(struct erofs_buf *buf, struct super_block *sb)
{
	buf->inode = sb->s_bdev->bd_inode;
}

void *erofs_read_metabuf(struct erofs_buf *buf, struct super_block *sb,
			 erofs_blk_t blkaddr, enum erofs_kmap_type type)
{
	erofs_init_metabuf(buf, sb);
	return erofs_bread(buf, blkaddr, type);
}

static int erofs_map_blocks_flatmode(struct inode *inode,
				     struct erofs_map_blocks *map)
{
	erofs_blk_t nblocks, lastblk;
	u64 offset = map->m_la;
	struct erofs_inode *vi = EROFS_I(inode);
	struct super_block *sb = inode->i_sb;
	bool tailendpacking = (vi->datalayout == EROFS_INODE_FLAT_INLINE);

	nblocks = erofs_iblks(inode);
	lastblk = nblocks - tailendpacking;

	/* there is no hole in flatmode */
	map->m_flags = EROFS_MAP_MAPPED;
	if (offset < erofs_pos(sb, lastblk)) {
		map->m_pa = erofs_pos(sb, vi->raw_blkaddr) + map->m_la;
		map->m_plen = erofs_pos(sb, lastblk) - offset;
	} else if (tailendpacking) {
		map->m_pa = erofs_iloc(inode) + vi->inode_isize +
			vi->xattr_isize + erofs_blkoff(sb, offset);
		map->m_plen = inode->i_size - offset;

		/* inline data should be located in the same meta block */
		if (erofs_blkoff(sb, map->m_pa) + map->m_plen > sb->s_blocksize) {
			erofs_err(sb, "inline data cross block boundary @ nid %llu",
				  vi->nid);
			DBG_BUGON(1);
			return -EFSCORRUPTED;
		}
		map->m_flags |= EROFS_MAP_META;
	} else {
		erofs_err(sb, "internal error @ nid: %llu (size %llu), m_la 0x%llx",
			  vi->nid, inode->i_size, map->m_la);
		DBG_BUGON(1);
		return -EIO;
	}
	return 0;
}

int erofs_map_blocks(struct inode *inode, struct erofs_map_blocks *map)
{
	struct super_block *sb = inode->i_sb;
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_inode_chunk_index *idx;
	struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
	u64 chunknr;
	unsigned int unit;
	erofs_off_t pos;
	void *kaddr;
	int err = 0;

	if (map->m_la >= inode->i_size) {
		/* leave out-of-bound access unmapped */
		map->m_flags = 0;
		map->m_plen = 0;
		goto out;
	}

	if (vi->datalayout != EROFS_INODE_CHUNK_BASED) {
		err = erofs_map_blocks_flatmode(inode, map);
		goto out;
	}

	if (vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES)
		unit = sizeof(*idx);			/* chunk index */
	else
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;	/* block map */

	chunknr = map->m_la >> vi->chunkbits;
	pos = ALIGN(erofs_iloc(inode) + vi->inode_isize +
		    vi->xattr_isize, unit) + unit * chunknr;

	kaddr = erofs_read_metabuf(&buf, sb, pos, EROFS_KMAP);
	if (IS_ERR(kaddr)) {
		err = PTR_ERR(kaddr);
		goto out;
	}
	map->m_la = chunknr << vi->chunkbits;
	map->m_plen = min_t(erofs_off_t, 1UL << vi->chunkbits,
			round_up(inode->i_size - map->m_la, sb->s_blocksize));

	/* handle block map */
	if (!(vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES)) {
		__le32 *blkaddr = kaddr;

		if (le32_to_cpu(*blkaddr) == EROFS_NULL_ADDR) {
			map->m_flags = 0;
		} else {
			map->m_pa = erofs_pos(sb, le32_to_cpu(*blkaddr));
			map->m_flags = EROFS_MAP_MAPPED;
		}
		goto out_unlock;
	}
	/* parse chunk indexes */
	idx = kaddr;
	switch (le32_to_cpu(idx->blkaddr)) {
	case EROFS_NULL_ADDR:
		map->m_flags = 0;
		break;
	default:
		/* only one device is supported for now */
		if (idx->device_id) {
			erofs_err(sb, "invalid device id %u @ %llu for nid %llu",
				  le16_to_cpu(idx->device_id), chunknr, vi->nid);
			err = -EFSCORRUPTED;
			goto out_unlock;
		}
		map->m_pa = erofs_pos(sb, le32_to_cpu(idx->blkaddr));
		map->m_flags = EROFS_MAP_MAPPED;
		break;
	}
out_unlock:
	erofs_put_metabuf(&buf);
out:
	map->m_llen = map->m_plen;
	return 0;
}

int erofs_get_block(struct inode *inode, sector_t iblock,
		    struct buffer_head *bh, int create)
{
	unsigned int bsz = 1U << inode->i_blkbits;
	struct super_block *sb = inode->i_sb;
	struct erofs_map_blocks map;
	int ret;

	map.m_la = iblock << inode->i_blkbits;
	map.m_llen = round_up(bh->b_size, bsz);
	ret = erofs_map_blocks(inode, &map);
	if (ret < 0)
		return ret;
	bh->b_size = round_up(map.m_llen, bsz);

	if (!(map.m_flags & EROFS_MAP_MAPPED)) {
		set_buffer_uptodate(bh);
		return 0;
	}

	if (map.m_flags & EROFS_MAP_META) {
		struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
		const unsigned int lo = map.m_la & (~PAGE_MASK);
		void *src, *dst;

		src = erofs_read_metabuf(&buf, sb, erofs_blknr(sb, map.m_pa),
					 EROFS_KMAP_ATOMIC);
		if (IS_ERR(src))
			return PTR_ERR(src);
		dst = kmap_atomic(bh->b_page);
		memcpy(dst + lo, src + erofs_blkoff(sb, map.m_pa), map.m_llen);
		memset(dst + lo + map.m_llen, 0, bh->b_size - map.m_llen);
		kunmap_atomic(dst);
		erofs_put_metabuf(&buf);
		bh->b_blocknr = 0;
		set_buffer_mapped(bh);
		set_buffer_uptodate(bh);
		return 0;
	}
	map_bh(bh, inode->i_sb, map.m_pa >> inode->i_blkbits);
	set_buffer_mapped(bh);
	return 0;
}

static int erofs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, erofs_get_block);
}

static int erofs_readpages(struct file *file, struct address_space *mapping,
		struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, erofs_get_block);
}

const struct address_space_operations erofs_raw_access_aops = {
	.readpage = erofs_readpage,
	.readpages = erofs_readpages,
};
