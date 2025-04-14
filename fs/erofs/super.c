// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/parser.h>
#include "internal.h"
#include "xattr.h"

static struct kmem_cache *erofs_inode_cachep __read_mostly;

void _erofs_err(struct super_block *sb, const char *func, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (sb)
		pr_err("(device %s): %s: %pV", sb->s_id, func, &vaf);
	else
		pr_err("%s: %pV", func, &vaf);
	va_end(args);
}

void _erofs_info(struct super_block *sb, const char *func, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	if (sb)
		pr_info("(device %s): %pV", sb->s_id, &vaf);
	else
		pr_info("%pV", &vaf);
	va_end(args);
}

static void erofs_inode_init_once(void *ptr)
{
	struct erofs_inode *vi = ptr;

	inode_init_once(&vi->vfs_inode);
}

static struct inode *erofs_alloc_inode(struct super_block *sb)
{
	struct erofs_inode *vi =
		kmem_cache_alloc(erofs_inode_cachep, GFP_KERNEL);

	if (!vi)
		return NULL;

	/* zero out everything except vfs_inode */
	memset(vi, 0, offsetof(struct erofs_inode, vfs_inode));
	return &vi->vfs_inode;
}

static void i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(erofs_inode_cachep, EROFS_I(inode));
}

static void erofs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, i_callback);
}

static int erofs_read_superblock(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
	struct erofs_super_block *dsb;
	void *data;
	int ret;

	data = erofs_read_metabuf(&buf, sb, 0, EROFS_KMAP_ATOMIC);
	if (IS_ERR(data)) {
		erofs_err(sb, "cannot read erofs superblock");
		return PTR_ERR(data);
	}
	dsb = (struct erofs_super_block *)(data + EROFS_SUPER_OFFSET);

	ret = -EINVAL;
	if (le32_to_cpu(dsb->magic) != EROFS_SUPER_MAGIC_V1) {
		erofs_err(sb, "cannot find valid erofs superblock");
		goto out;
	}

	sbi->blkszbits = dsb->blkszbits;
	if (sbi->blkszbits < 9 || sbi->blkszbits > PAGE_SHIFT) {
		erofs_err(sb, "blkszbits %u isn't supported", sbi->blkszbits);
		goto out;
	}

	sbi->total_blocks = le32_to_cpu(dsb->blocks);
	sbi->meta_blkaddr = le32_to_cpu(dsb->meta_blkaddr);
#ifdef CONFIG_EROFS_FS_XATTR
	sbi->xattr_blkaddr = le32_to_cpu(dsb->xattr_blkaddr);
#endif
	sbi->islotbits = ilog2(sizeof(struct erofs_inode_compact));
	sbi->root_nid = le16_to_cpu(dsb->root_nid);
	sbi->inos = le64_to_cpu(dsb->inos);

	sbi->build_time = le64_to_cpu(dsb->build_time);
	sbi->build_time_nsec = le32_to_cpu(dsb->build_time_nsec);

	memcpy(&sb->s_uuid, dsb->uuid, sizeof(dsb->uuid));
	memcpy(sbi->volume_name, dsb->volume_name, sizeof(dsb->volume_name));
	ret = 0;
out:
	erofs_put_metabuf(&buf);
	return ret;
}

static void erofs_default_options(struct erofs_sb_info *sbi)
{
	set_opt(sbi, XATTR_USER);
}

enum {
	Opt_user_xattr,
	Opt_nouser_xattr,
	Opt_err
};

static match_table_t erofs_tokens = {
	{Opt_user_xattr, "user_xattr"},
	{Opt_nouser_xattr, "nouser_xattr"},
	{Opt_err, NULL}
};

static int erofs_parse_options(struct super_block *sb, char *options)
{
	substring_t args[MAX_OPT_ARGS];
	char *p;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;

		args[0].to = args[0].from = NULL;
		token = match_token(p, erofs_tokens, args);

		switch (token) {
#ifdef CONFIG_EROFS_FS_XATTR
		case Opt_user_xattr:
			set_opt(EROFS_SB(sb), XATTR_USER);
			break;
		case Opt_nouser_xattr:
			clear_opt(EROFS_SB(sb), XATTR_USER);
			break;
#else
		case Opt_user_xattr:
			infoln("user_xattr options not supported");
			break;
		case Opt_nouser_xattr:
			infoln("nouser_xattr options not supported");
			break;
#endif
		default:
			erofs_err(sb, "Unrecognized mount option \"%s\" or missing value", p);
			return -EINVAL;
		}
	}
	return 0;
}

static int erofs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct erofs_sb_info *sbi;
	struct inode *inode;
	int err;

	sb->s_magic = EROFS_SUPER_MAGIC;
	sb->s_flags |= MS_RDONLY | MS_NOATIME;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &erofs_sops;

	sbi = kzalloc(sizeof(struct erofs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	if (!sb_set_blocksize(sb, PAGE_SIZE)) {
		erofs_err(sb, "failed to set initial blksize");
		return -EINVAL;
	}

	err = erofs_read_superblock(sb);
	if (err)
		return err;

	if (sb->s_blocksize_bits != sbi->blkszbits) {
		if (!sb_set_blocksize(sb, 1 << sbi->blkszbits)) {
			erofs_err(sb, "failed to set erofs blksize");
			return -EINVAL;
		}
	}
	sb->s_time_gran = 1;
#ifdef CONFIG_EROFS_FS_XATTR
	sb->s_xattr = erofs_xattr_handlers;
#endif
	erofs_default_options(sbi);
	err = erofs_parse_options(sb, data);
	if (err)
		return err;

	inode = erofs_iget(sb, sbi->root_nid);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (!S_ISDIR(inode->i_mode)) {
		erofs_err(sb, "rootino(nid %llu) is not a directory(i_mode %o)",
			  sbi->root_nid, inode->i_mode);
		iput(inode);
		return -EINVAL;
	}

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	if (!silent)
		erofs_info(sb, "mounted with root inode @ nid %llu.", sbi->root_nid);
	return 0;
}

static struct dentry *erofs_mount(struct file_system_type *fs_type, int flags,
				  const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, erofs_fill_super);
}

static void erofs_kill_sb(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);

	kill_block_super(sb);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static struct file_system_type erofs_fs_type = {
	.owner          = THIS_MODULE,
	.name           = "erofs",
	.mount          = erofs_mount,
	.kill_sb        = erofs_kill_sb,
	.fs_flags       = FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("erofs");

static int __init erofs_module_init(void)
{
	int err;

	erofs_check_ondisk_layout_definitions();
	erofs_inode_cachep = kmem_cache_create("erofs_inode",
			sizeof(struct erofs_inode), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
			erofs_inode_init_once);
	if (!erofs_inode_cachep)
		return -ENOMEM;

	err = register_filesystem(&erofs_fs_type);
	if (err)
		goto fs_err;
	return 0;

fs_err:
	kmem_cache_destroy(erofs_inode_cachep);
	return err;
}

static void __exit erofs_module_exit(void)
{
	unregister_filesystem(&erofs_fs_type);

	/* Ensure all RCU free inodes / pclusters are safe to be destroyed. */
	rcu_barrier();
	kmem_cache_destroy(erofs_inode_cachep);
}

static int erofs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_blocksize;
	buf->f_type = sb->s_magic;
	buf->f_blocks = sbi->total_blocks;
	buf->f_bfree = buf->f_bavail = 0;

	buf->f_files = ULLONG_MAX;
	buf->f_ffree = ULLONG_MAX - sbi->inos;

	buf->f_namelen = EROFS_NAME_LEN;

	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	return 0;
}

static int erofs_remount(struct super_block *sb, int *flags, char *data)
{
	*flags |= MS_RDONLY;
	return 0;
}

const struct super_operations erofs_sops = {
	.alloc_inode = erofs_alloc_inode,
	.destroy_inode = erofs_destroy_inode,
	.statfs = erofs_statfs,
	.remount_fs = erofs_remount,
};

module_init(erofs_module_init);
module_exit(erofs_module_exit);

MODULE_DESCRIPTION("Enhanced ROM File System");
MODULE_AUTHOR("Gao Xiang, Yu Chao, Miao Xie, CONSUMER BG, HUAWEI Inc.");
MODULE_LICENSE("GPL");
