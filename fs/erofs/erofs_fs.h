/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * EROFS (Enhanced ROM File System) on-disk format definition
 *
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#ifndef __EROFS_FS_H
#define __EROFS_FS_H

/* Enhanced(Extended) ROM File System */
#define EROFS_SUPER_MAGIC_V1    0xE0F5E1E2
#define EROFS_SUPER_OFFSET      1024

struct erofs_super_block {
	__le32 magic;           /* file system magic number */
	__le32 checksum;        /* crc32c(super_block) */
	__le32 feature_compat;
	__u8 blkszbits;         /* filesystem block size in bit shift */
	__u8 reserved;

	__le16 root_nid;	/* nid of root directory */
	__le64 inos;            /* total valid ino # (== f_files - f_favail) */

	__le64 build_time;      /* compact inode time derivation */
	__le32 build_time_nsec;	/* compact inode time derivation in ns scale */
	__le32 blocks;          /* used for statfs */
	__le32 meta_blkaddr;	/* start block address of metadata area */
	__le32 xattr_blkaddr;	/* start block address of shared xattr area */
	__u8 uuid[16];          /* 128-bit uuid for volume */
	__u8 volume_name[16];   /* volume name */
	__le32 feature_incompat;
	__u8 reserved2[44];
};

/*
 * EROFS inode datalayout (i_format in on-disk inode):
 * 0 - uncompressed flat inode without tail-packing inline data:
 * 2 - uncompressed flat inode with tail-packing inline data:
 * 4 - chunk-based inode with (optional) multi-device support:
 * 5~7 - reserved
 */
enum {
	EROFS_INODE_FLAT_PLAIN			= 0,
	EROFS_INODE_FLAT_INLINE			= 2,
	EROFS_INODE_CHUNK_BASED			= 4,
	EROFS_INODE_DATALAYOUT_MAX
};

/* bit definitions of inode i_format */
#define EROFS_I_VERSION_MASK            0x01
#define EROFS_I_DATALAYOUT_MASK         0x07

#define EROFS_I_VERSION_BIT             0
#define EROFS_I_DATALAYOUT_BIT          1
#define EROFS_I_ALL_BIT			4

#define EROFS_I_ALL	((1 << EROFS_I_ALL_BIT) - 1)

struct erofs_inode_chunk_info {
	__le16 format;		/* chunk blkbits, etc. */
	__le16 reserved;
};

/* indicate chunk blkbits, thus 'chunksize = blocksize << chunk blkbits' */
#define EROFS_CHUNK_FORMAT_BLKBITS_MASK		0x001F
/* with chunk indexes or just a 4-byte blkaddr array */
#define EROFS_CHUNK_FORMAT_INDEXES		0x0020

#define EROFS_CHUNK_FORMAT_ALL	\
	(EROFS_CHUNK_FORMAT_BLKBITS_MASK | EROFS_CHUNK_FORMAT_INDEXES)

/* 32-byte on-disk inode */
#define EROFS_INODE_LAYOUT_COMPACT	0
/* 64-byte on-disk inode */
#define EROFS_INODE_LAYOUT_EXTENDED	1

union erofs_inode_i_u {
	/* total compressed blocks for compressed inodes */
	__le32 compressed_blocks;

	/* block address for uncompressed flat inodes */
	__le32 raw_blkaddr;

	/* for device files, used to indicate old/new device # */
	__le32 rdev;

	/* for chunk-based files, it contains the summary info */
	struct erofs_inode_chunk_info c;
};

/* 32-byte reduced form of an ondisk inode */
struct erofs_inode_compact {
	__le16 i_format;	/* inode format hints */

/* 1 header + n-1 * 4 bytes inline xattr to keep continuity */
	__le16 i_xattr_icount;
	__le16 i_mode;
	__le16 i_nlink;
	__le32 i_size;
	__le32 i_reserved;
	union erofs_inode_i_u i_u;

	__le32 i_ino;		/* only used for 32-bit stat compatibility */
	__le16 i_uid;
	__le16 i_gid;
	__le32 i_reserved2;
};

/* 64-byte complete form of an ondisk inode */
struct erofs_inode_extended {
	__le16 i_format;	/* inode format hints */

/* 1 header + n-1 * 4 bytes inline xattr to keep continuity */
	__le16 i_xattr_icount;
	__le16 i_mode;
	__le16 i_reserved;
	__le64 i_size;
	union erofs_inode_i_u i_u;

	__le32 i_ino;		/* only used for 32-bit stat compatibility */
	__le32 i_uid;
	__le32 i_gid;
	__le64 i_mtime;
	__le32 i_mtime_nsec;
	__le32 i_nlink;
	__u8   i_reserved2[16];
};

/*
 * inline xattrs (n == i_xattr_icount):
 * erofs_xattr_ibody_header(1) + (n - 1) * 4 bytes
 *          12 bytes           /                   \
 *                            /                     \
 *                           /-----------------------\
 *                           |  erofs_xattr_entries+ |
 *                           +-----------------------+
 * inline xattrs must starts in erofs_xattr_ibody_header,
 * for read-only fs, no need to introduce h_refcount
 */
struct erofs_xattr_ibody_header {
	__le32 h_name_filter;		/* bit value 1 indicates not-present */
	__u8   h_shared_count;
	__u8   h_reserved2[7];
	__le32 h_shared_xattrs[];       /* shared xattr id array */
};

/* Name indexes */
#define EROFS_XATTR_INDEX_USER              1
#define EROFS_XATTR_INDEX_POSIX_ACL_ACCESS  2
#define EROFS_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EROFS_XATTR_INDEX_TRUSTED           4
#define EROFS_XATTR_INDEX_LUSTRE            5
#define EROFS_XATTR_INDEX_SECURITY          6

/* xattr entry (for both inline & shared xattrs) */
struct erofs_xattr_entry {
	__u8   e_name_len;      /* length of name */
	__u8   e_name_index;    /* attribute name index */
	__le16 e_value_size;    /* size of attribute value */
	/* followed by e_name and e_value */
	char   e_name[];        /* attribute name */
};

/* long xattr name prefix */
struct erofs_xattr_long_prefix {
	__u8   base_index;	/* short xattr name prefix index */
	char   infix[];		/* infix apart from short prefix */
};

static inline unsigned int erofs_xattr_ibody_size(__le16 i_xattr_icount)
{
	if (!i_xattr_icount)
		return 0;

	return sizeof(struct erofs_xattr_ibody_header) +
		sizeof(__u32) * (le16_to_cpu(i_xattr_icount) - 1);
}

#define EROFS_XATTR_ALIGN(size) round_up(size, sizeof(struct erofs_xattr_entry))

static inline unsigned int erofs_xattr_entry_size(struct erofs_xattr_entry *e)
{
	return EROFS_XATTR_ALIGN(sizeof(struct erofs_xattr_entry) +
				 e->e_name_len + le16_to_cpu(e->e_value_size));
}

/* represent a zeroed chunk (hole) */
#define EROFS_NULL_ADDR			-1

/* 4-byte block address array */
#define EROFS_BLOCK_MAP_ENTRY_SIZE	sizeof(__le32)

/* 8-byte inode chunk indexes */
struct erofs_inode_chunk_index {
	__le16 advise;		/* always 0, don't care for now */
	__le16 device_id;	/* back-end storage id (with bits masked) */
	__le32 blkaddr;		/* start block address of this inode chunk */
};

/* dirent sorts in alphabet order, thus we can do binary search */
struct erofs_dirent {
	__le64 nid;     /* node number */
	__le16 nameoff; /* start offset of file name */
	__u8 file_type; /* file type */
	__u8 reserved;  /* reserved */
} __packed;

/* file types used in inode_info->flags */
enum {
	EROFS_FT_UNKNOWN,
	EROFS_FT_REG_FILE,
	EROFS_FT_DIR,
	EROFS_FT_CHRDEV,
	EROFS_FT_BLKDEV,
	EROFS_FT_FIFO,
	EROFS_FT_SOCK,
	EROFS_FT_SYMLINK,
	EROFS_FT_MAX
};

#define EROFS_NAME_LEN      255

/* check the EROFS on-disk layout strictly at compile time */
static inline void erofs_check_ondisk_layout_definitions(void)
{
	BUILD_BUG_ON(sizeof(struct erofs_super_block) != 128);
	BUILD_BUG_ON(sizeof(struct erofs_inode_compact) != 32);
	BUILD_BUG_ON(sizeof(struct erofs_inode_extended) != 64);
	BUILD_BUG_ON(sizeof(struct erofs_inode_chunk_info) != 4);
	BUILD_BUG_ON(sizeof(struct erofs_inode_chunk_index) != 8);
	BUILD_BUG_ON(sizeof(struct erofs_dirent) != 12);
}

#endif
