// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021-2022, Alibaba Cloud
 */
#include <linux/security.h>
#include "xattr.h"

struct erofs_xattr_iter {
	struct super_block *sb;
	struct erofs_buf buf;
	erofs_off_t pos;
	void *kaddr;

	char *buffer;
	int buffer_size, buffer_ofs;

	/* getxattr */
	int index;
	struct qstr name;

	/* listxattr */
	struct dentry *dentry;
};

static int erofs_init_inode_xattrs(struct inode *inode)
{
	struct erofs_inode *const vi = EROFS_I(inode);
	struct erofs_xattr_iter it;
	unsigned int i;
	struct erofs_xattr_ibody_header *ih;
	struct super_block *sb = inode->i_sb;
	int ret = 0;

	/* the most case is that xattrs of this inode are initialized. */
	if (test_bit(EROFS_I_EA_INITED_BIT, &vi->flags)) {
		/*
		 * paired with smp_mb() at the end of the function to ensure
		 * fields will only be observed after the bit is set.
		 */
		smp_mb();
		return 0;
	}

	if (wait_on_bit_lock(&vi->flags, EROFS_I_BL_XATTR_BIT, TASK_KILLABLE))
		return -ERESTARTSYS;

	/* someone has initialized xattrs for us? */
	if (test_bit(EROFS_I_EA_INITED_BIT, &vi->flags))
		goto out_unlock;

	/*
	 * bypass all xattr operations if ->xattr_isize is not greater than
	 * sizeof(struct erofs_xattr_ibody_header), in detail:
	 * 1) it is not enough to contain erofs_xattr_ibody_header then
	 *    ->xattr_isize should be 0 (it means no xattr);
	 * 2) it is just to contain erofs_xattr_ibody_header, which is on-disk
	 *    undefined right now (maybe use later with some new sb feature).
	 */
	if (vi->xattr_isize == sizeof(struct erofs_xattr_ibody_header)) {
		erofs_err(sb,
			  "xattr_isize %d of nid %llu is not supported yet",
			  vi->xattr_isize, vi->nid);
		ret = -EOPNOTSUPP;
		goto out_unlock;
	} else if (vi->xattr_isize < sizeof(struct erofs_xattr_ibody_header)) {
		if (vi->xattr_isize) {
			erofs_err(sb, "bogus xattr ibody @ nid %llu", vi->nid);
			DBG_BUGON(1);
			ret = -EFSCORRUPTED;
			goto out_unlock;	/* xattr ondisk layout error */
		}
		ret = -ENOATTR;
		goto out_unlock;
	}

	it.buf = __EROFS_BUF_INITIALIZER;
	erofs_init_metabuf(&it.buf, sb);
	it.pos = erofs_iloc(inode) + vi->inode_isize;

	/* read in shared xattr array (non-atomic, see kmalloc below) */
	it.kaddr = erofs_bread(&it.buf, erofs_blknr(sb, it.pos), EROFS_KMAP);
	if (IS_ERR(it.kaddr)) {
		ret = PTR_ERR(it.kaddr);
		goto out_unlock;
	}

	ih = it.kaddr + erofs_blkoff(sb, it.pos);
	vi->xattr_shared_count = ih->h_shared_count;
	vi->xattr_shared_xattrs = kmalloc_array(vi->xattr_shared_count,
						sizeof(uint), GFP_KERNEL);
	if (!vi->xattr_shared_xattrs) {
		erofs_put_metabuf(&it.buf);
		ret = -ENOMEM;
		goto out_unlock;
	}

	/* let's skip ibody header */
	it.pos += sizeof(struct erofs_xattr_ibody_header);

	for (i = 0; i < vi->xattr_shared_count; ++i) {
		it.kaddr = erofs_bread(&it.buf, erofs_blknr(sb, it.pos),
				       EROFS_KMAP);
		if (IS_ERR(it.kaddr)) {
			kfree(vi->xattr_shared_xattrs);
			vi->xattr_shared_xattrs = NULL;
			ret = PTR_ERR(it.kaddr);
			goto out_unlock;
		}
		vi->xattr_shared_xattrs[i] = le32_to_cpu(*(__le32 *)
				(it.kaddr + erofs_blkoff(sb, it.pos)));
		it.pos += sizeof(__le32);
	}
	erofs_put_metabuf(&it.buf);

	/* paired with smp_mb() at the beginning of the function. */
	smp_mb();
	set_bit(EROFS_I_EA_INITED_BIT, &vi->flags);

out_unlock:
	clear_and_wake_up_bit(EROFS_I_BL_XATTR_BIT, &vi->flags);
	return ret;
}

static int erofs_xattr_copy_to_buffer(struct erofs_xattr_iter *it,
				      unsigned int len)
{
	unsigned int slice, processed;
	struct super_block *sb = it->sb;
	void *src;

	for (processed = 0; processed < len; processed += slice) {
		it->kaddr = erofs_bread(&it->buf, erofs_blknr(sb, it->pos),
					EROFS_KMAP);
		if (IS_ERR(it->kaddr))
			return PTR_ERR(it->kaddr);

		src = it->kaddr + erofs_blkoff(sb, it->pos);
		slice = min_t(unsigned int, sb->s_blocksize -
				erofs_blkoff(sb, it->pos), len - processed);
		memcpy(it->buffer + it->buffer_ofs, src, slice);
		it->buffer_ofs += slice;
		it->pos += slice;
	}
	return 0;
}

static int erofs_listxattr_foreach(struct erofs_xattr_iter *it)
{
	struct erofs_xattr_entry entry;
	unsigned int base_index, name_total, prefix_len;
	const char *prefix;
	int err;
	const struct xattr_handler *h;

	/* 1. handle xattr entry */
	entry = *(struct erofs_xattr_entry *)
			(it->kaddr + erofs_blkoff(it->sb, it->pos));
	base_index = entry.e_name_index;

	h = erofs_xattr_handler(base_index);
	if (!h || (h->list && !h->list(it->dentry, NULL, 0,
				NULL, entry.e_name_len, base_index)))
		return 0;
	it->pos += sizeof(struct erofs_xattr_entry);
	prefix = h->prefix;
	prefix_len = strlen(prefix);
	name_total = prefix_len + entry.e_name_len + 1;

	if (!it->buffer) {
		it->buffer_ofs += name_total;
		return 0;
	}

	if (it->buffer_ofs + name_total > it->buffer_size)
		return -ERANGE;

	memcpy(it->buffer + it->buffer_ofs, prefix, prefix_len);
	it->buffer_ofs += prefix_len;

	/* 2. handle xattr name */
	err = erofs_xattr_copy_to_buffer(it, entry.e_name_len);
	if (err)
		return err;

	it->buffer[it->buffer_ofs++] = '\0';
	return 0;
}

static int erofs_getxattr_foreach(struct erofs_xattr_iter *it)
{
	struct super_block *sb = it->sb;
	struct erofs_xattr_entry entry;
	unsigned int slice, processed, value_sz;

	/* 1. handle xattr entry */
	entry = *(struct erofs_xattr_entry *)
			(it->kaddr + erofs_blkoff(sb, it->pos));
	it->pos += sizeof(struct erofs_xattr_entry);
	value_sz = le16_to_cpu(entry.e_value_size);

	if (it->index != entry.e_name_index ||
	    it->name.len != entry.e_name_len)
		return -ENOATTR;

	/* 2. handle xattr name */
	for (processed = 0; processed < entry.e_name_len; processed += slice) {
		it->kaddr = erofs_bread(&it->buf, erofs_blknr(sb, it->pos),
					EROFS_KMAP);
		if (IS_ERR(it->kaddr))
			return PTR_ERR(it->kaddr);

		slice = min_t(unsigned int,
				sb->s_blocksize - erofs_blkoff(sb, it->pos),
				entry.e_name_len - processed);
		if (memcmp(it->name.name + processed,
			   it->kaddr + erofs_blkoff(sb, it->pos), slice))
			return -ENOATTR;
		it->pos += slice;
	}

	/* 3. handle xattr value */
	if (!it->buffer) {
		it->buffer_ofs = value_sz;
		return 0;
	}

	if (it->buffer_size < value_sz)
		return -ERANGE;

	return erofs_xattr_copy_to_buffer(it, value_sz);
}

static int erofs_xattr_iter_inline(struct erofs_xattr_iter *it,
				   struct inode *inode, bool getxattr)
{
	struct erofs_inode *const vi = EROFS_I(inode);
	unsigned int xattr_header_sz, remaining, entry_sz;
	erofs_off_t next_pos;
	int ret;

	xattr_header_sz = sizeof(struct erofs_xattr_ibody_header) +
			  sizeof(u32) * vi->xattr_shared_count;
	if (xattr_header_sz >= vi->xattr_isize) {
		DBG_BUGON(xattr_header_sz > vi->xattr_isize);
		return -ENOATTR;
	}

	remaining = vi->xattr_isize - xattr_header_sz;
	it->pos = erofs_iloc(inode) + vi->inode_isize + xattr_header_sz;

	do {
		it->kaddr = erofs_bread(&it->buf, erofs_blknr(it->sb, it->pos),
					EROFS_KMAP);
		if (IS_ERR(it->kaddr))
			return PTR_ERR(it->kaddr);

		entry_sz = erofs_xattr_entry_size(it->kaddr +
				erofs_blkoff(it->sb, it->pos));
		/* xattr on-disk corruption: xattr entry beyond xattr_isize */
		if (remaining < entry_sz) {
			DBG_BUGON(1);
			return -EFSCORRUPTED;
		}
		remaining -= entry_sz;
		next_pos = it->pos + entry_sz;

		if (getxattr)
			ret = erofs_getxattr_foreach(it);
		else
			ret = erofs_listxattr_foreach(it);
		if ((getxattr && ret != -ENOATTR) || (!getxattr && ret))
			break;

		it->pos = next_pos;
	} while (remaining);
	return ret;
}

static int erofs_xattr_iter_shared(struct erofs_xattr_iter *it,
				   struct inode *inode, bool getxattr)
{
	struct erofs_inode *const vi = EROFS_I(inode);
	struct super_block *const sb = it->sb;
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	unsigned int i;
	int ret = -ENOATTR;

	for (i = 0; i < vi->xattr_shared_count; ++i) {
		it->pos = erofs_pos(sb, sbi->xattr_blkaddr) +
				vi->xattr_shared_xattrs[i] * sizeof(__le32);
		it->kaddr = erofs_bread(&it->buf, erofs_blknr(sb, it->pos),
					EROFS_KMAP);
		if (IS_ERR(it->kaddr))
			return PTR_ERR(it->kaddr);

		if (getxattr)
			ret = erofs_getxattr_foreach(it);
		else
			ret = erofs_listxattr_foreach(it);
		if ((getxattr && ret != -ENOATTR) || (!getxattr && ret))
			break;
	}
	return ret;
}

int erofs_getxattr(struct inode *inode, int index, const char *name,
		   void *buffer, size_t buffer_size)
{
	int ret;
	struct erofs_xattr_iter it;

	if (!name)
		return -EINVAL;

	ret = erofs_init_inode_xattrs(inode);
	if (ret)
		return ret;

	it.index = index;
	it.name = (struct qstr)QSTR_INIT(name, strlen(name));
	if (it.name.len > EROFS_NAME_LEN)
		return -ERANGE;

	it.sb = inode->i_sb;
	it.buf = __EROFS_BUF_INITIALIZER;
	erofs_init_metabuf(&it.buf, it.sb);
	it.buffer = buffer;
	it.buffer_size = buffer_size;
	it.buffer_ofs = 0;

	ret = erofs_xattr_iter_inline(&it, inode, true);
	if (ret == -ENOATTR)
		ret = erofs_xattr_iter_shared(&it, inode, true);
	erofs_put_metabuf(&it.buf);
	return ret ? ret : it.buffer_ofs;
}

ssize_t erofs_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int ret;
	struct erofs_xattr_iter it;
	struct inode *inode = d_inode(dentry);

	ret = erofs_init_inode_xattrs(inode);
	if (ret == -ENOATTR)
		return 0;
	if (ret)
		return ret;

	it.sb = dentry->d_sb;
	it.buf = __EROFS_BUF_INITIALIZER;
	erofs_init_metabuf(&it.buf, it.sb);
	it.dentry = dentry;
	it.buffer = buffer;
	it.buffer_size = buffer_size;
	it.buffer_ofs = 0;

	ret = erofs_xattr_iter_inline(&it, inode, false);
	if (!ret || ret == -ENOATTR)
		ret = erofs_xattr_iter_shared(&it, inode, false);
	if (ret == -ENOATTR)
		ret = 0;
	erofs_put_metabuf(&it.buf);
	return ret ? ret : it.buffer_ofs;
}

static size_t erofs_xattr_generic_list(struct dentry *dentry, char *list,
		size_t list_size, const char *name, size_t name_len, int type)
{
	struct erofs_sb_info *sbi = EROFS_SB(dentry->d_sb);
	int total_len, prefix_len = 0;
	const char *prefix = NULL;

	switch (type) {
	case EROFS_XATTR_INDEX_USER:
		if (!test_opt(sbi, XATTR_USER))
			return 0;
		prefix = XATTR_USER_PREFIX;
		prefix_len = XATTR_USER_PREFIX_LEN;
		break;
	case EROFS_XATTR_INDEX_TRUSTED:
		if (!capable(CAP_SYS_ADMIN))
			return 0;
		prefix = XATTR_TRUSTED_PREFIX;
		prefix_len = XATTR_TRUSTED_PREFIX_LEN;
		break;
	default:
		return 0;
	}

	total_len = prefix_len + name_len + 1;
	if (list && total_len <= list_size) {
		memcpy(list, prefix, prefix_len);
		memcpy(list+prefix_len, name, name_len);
		list[prefix_len + name_len] = '\0';
	}
	return total_len;
}

static int erofs_xattr_generic_get(struct dentry *dentry, const char *name,
		void *buffer, size_t size, int type)

{
	struct inode *inode = dentry->d_inode;

	if (type == EROFS_XATTR_INDEX_USER &&
	    !test_opt(EROFS_I_SB(inode), XATTR_USER))
		return -EOPNOTSUPP;
	return erofs_getxattr(inode, type, name, buffer, size);
}

const struct xattr_handler erofs_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.flags	= EROFS_XATTR_INDEX_USER,
	.list	= erofs_xattr_generic_list,
	.get	= erofs_xattr_generic_get,
};

const struct xattr_handler erofs_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.flags	= EROFS_XATTR_INDEX_TRUSTED,
	.list	= erofs_xattr_generic_list,
	.get	= erofs_xattr_generic_get,
};

#ifdef CONFIG_EROFS_FS_SECURITY
const struct xattr_handler __maybe_unused erofs_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.flags	= EROFS_XATTR_INDEX_SECURITY,
	.get	= erofs_xattr_generic_get,
};
#endif

const struct xattr_handler *erofs_xattr_handlers[] = {
	&erofs_xattr_user_handler,
	&erofs_xattr_trusted_handler,
#ifdef CONFIG_EROFS_FS_SECURITY
	&erofs_xattr_security_handler,
#endif
	NULL,
};
