/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#ifndef __INTERNAL_H
#define __INTERNAL_H

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "erofs_fs.h"
#include "compat.h"

/* redefine pr_fmt "erofs: " */
#undef pr_fmt
#define pr_fmt(fmt) "erofs: " fmt

__printf(3, 4) void _erofs_err(struct super_block *sb,
			       const char *function, const char *fmt, ...);
#define erofs_err(sb, fmt, ...)	\
	_erofs_err(sb, __func__, fmt "\n", ##__VA_ARGS__)
__printf(3, 4) void _erofs_info(struct super_block *sb,
			       const char *function, const char *fmt, ...);
#define erofs_info(sb, fmt, ...) \
	_erofs_info(sb, __func__, fmt "\n", ##__VA_ARGS__)
#ifdef CONFIG_EROFS_FS_DEBUG
#define DBG_BUGON               BUG_ON
#else
#define DBG_BUGON(x)            ((void)(x))
#endif	/* !CONFIG_EROFS_FS_DEBUG */

/* EROFS_SUPER_MAGIC_V1 to represent the whole file system */
#define EROFS_SUPER_MAGIC   EROFS_SUPER_MAGIC_V1

typedef u64 erofs_nid_t;
typedef u64 erofs_off_t;
/* data type for filesystem-wide blocks number */
typedef u32 erofs_blk_t;

struct erofs_sb_info {
	u32 total_blocks;
	u32 meta_blkaddr;
#ifdef CONFIG_EROFS_FS_XATTR
	u32 xattr_blkaddr;
#endif

	unsigned char islotbits;	/* inode slot unit size in bit shift */
	unsigned char blkszbits;	/* filesystem block size in bit shift */

	u32 build_time_nsec;
	u64 build_time;

	/* what we really care is nid, rather than ino.. */
	erofs_nid_t root_nid;
	/* used for statfs, f_files - f_favail */
	u64 inos;

	u8 uuid[16];                    /* 128-bit uuid for volume */
	u8 volume_name[16];             /* volume name */

	unsigned int mount_opt;
};

#define EROFS_SB(sb) ((struct erofs_sb_info *)(sb)->s_fs_info)
#define EROFS_I_SB(inode) ((struct erofs_sb_info *)(inode)->i_sb->s_fs_info)

/* Mount flags set via mount options or defaults */
#define EROFS_MOUNT_XATTR_USER		0x00000010

#define clear_opt(sbi, option)	((sbi)->mount_opt &= ~EROFS_MOUNT_##option)
#define set_opt(sbi, option)	((sbi)->mount_opt |= EROFS_MOUNT_##option)
#define test_opt(sbi, option)	((sbi)->mount_opt & EROFS_MOUNT_##option)

enum erofs_kmap_type {
	EROFS_NO_KMAP,		/* don't map the buffer */
	EROFS_KMAP,		/* use kmap() to map the buffer */
	EROFS_KMAP_ATOMIC,	/* use kmap_atomic() to map the buffer */
};

struct erofs_buf {
	struct inode *inode;
	struct page *page;
	void *base;
	enum erofs_kmap_type kmap_type;
};
#define __EROFS_BUF_INITIALIZER	((struct erofs_buf){ .page = NULL })

#define erofs_blknr(sb, addr)	((addr) >> (sb)->s_blocksize_bits)
#define erofs_blkoff(sb, addr)	((addr) & ((sb)->s_blocksize - 1))
#define erofs_pos(sb, blk)	((erofs_off_t)(blk) << (sb)->s_blocksize_bits)
#define erofs_iblks(i)	(round_up((i)->i_size, (i)->i_sb->s_blocksize) >> (i)->i_blkbits)

/* atomic flag definitions */
#define EROFS_I_EA_INITED_BIT	0

/* bitlock definitions (arranged in reverse order) */
#define EROFS_I_BL_XATTR_BIT	(BITS_PER_LONG - 1)

struct erofs_inode {
	erofs_nid_t nid;
	unsigned long flags;	/* atomic flags (including bitlocks) */

	unsigned char datalayout;
	unsigned char inode_isize;
	unsigned int xattr_isize;

	unsigned int xattr_shared_count;
	unsigned int *xattr_shared_xattrs;

	union {
		erofs_blk_t raw_blkaddr;
		struct {
			unsigned short	chunkformat;
			unsigned char	chunkbits;
		};
	};

	/* the corresponding vfs inode */
	struct inode vfs_inode;
};

#define EROFS_I(ptr)	container_of(ptr, struct erofs_inode, vfs_inode)

static inline erofs_off_t erofs_iloc(struct inode *inode)
{
	struct erofs_sb_info *sbi = EROFS_I_SB(inode);

	return erofs_pos(inode->i_sb, sbi->meta_blkaddr) +
		(EROFS_I(inode)->nid << sbi->islotbits);
}

static inline unsigned int erofs_inode_version(unsigned int ifmt)
{
	return (ifmt >> EROFS_I_VERSION_BIT) & EROFS_I_VERSION_MASK;
}

static inline unsigned int erofs_inode_datalayout(unsigned int ifmt)
{
	return (ifmt >> EROFS_I_DATALAYOUT_BIT) & EROFS_I_DATALAYOUT_MASK;
}

extern const struct super_operations erofs_sops;
extern const struct inode_operations erofs_dir_iops;
extern const struct file_operations erofs_dir_fops;

extern const struct address_space_operations erofs_raw_access_aops;

/* Has a disk mapping */
#define EROFS_MAP_MAPPED	0x0001
/* Located in metadata (could be copied from bd_inode) */
#define EROFS_MAP_META		0x0002

struct erofs_map_blocks {
	erofs_off_t m_pa, m_la;
	u64 m_plen, m_llen;

	unsigned int m_flags;
};

/* Flags used by erofs_map_blocks() */
#define EROFS_GET_BLOCKS_RAW    0x0001

void *erofs_read_metadata(struct super_block *sb, struct erofs_buf *buf,
			  erofs_off_t *offset, int *lengthp);
void erofs_unmap_metabuf(struct erofs_buf *buf);
void erofs_put_metabuf(struct erofs_buf *buf);
void *erofs_bread(struct erofs_buf *buf, erofs_blk_t blkaddr,
		  enum erofs_kmap_type type);
void erofs_init_metabuf(struct erofs_buf *buf, struct super_block *sb);
void *erofs_read_metabuf(struct erofs_buf *buf, struct super_block *sb,
			 erofs_blk_t blkaddr, enum erofs_kmap_type type);
int erofs_map_blocks(struct inode *inode, struct erofs_map_blocks *map);

struct inode *erofs_iget(struct super_block *sb, erofs_nid_t nid);

int erofs_namei(struct inode *dir, const struct qstr *name,
		erofs_nid_t *nid, unsigned int *d_type);

#define EFSCORRUPTED    EUCLEAN         /* Filesystem is corrupted */

#endif
