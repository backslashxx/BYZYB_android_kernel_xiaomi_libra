/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 */
#ifndef __EROFS_XATTR_H
#define __EROFS_XATTR_H

#include "internal.h"
#include <linux/xattr.h>

/* Attribute not found */
#define ENOATTR         ENODATA

#ifdef CONFIG_EROFS_FS_XATTR
extern const struct xattr_handler erofs_xattr_user_handler;
extern const struct xattr_handler erofs_xattr_trusted_handler;
extern const struct xattr_handler erofs_xattr_security_handler;

static inline const struct xattr_handler *erofs_xattr_handler(unsigned int idx)
{
	static const struct xattr_handler * const xattr_handler_map[] = {
		[EROFS_XATTR_INDEX_USER] = &erofs_xattr_user_handler,
		[EROFS_XATTR_INDEX_TRUSTED] = &erofs_xattr_trusted_handler,
#ifdef CONFIG_EROFS_FS_SECURITY
		[EROFS_XATTR_INDEX_SECURITY] = &erofs_xattr_security_handler,
#endif
	};

	return idx && idx < ARRAY_SIZE(xattr_handler_map) ?
		xattr_handler_map[idx] : NULL;
}

extern const struct xattr_handler *erofs_xattr_handlers[];

int erofs_xattr_prefixes_init(struct super_block *sb);
void erofs_xattr_prefixes_cleanup(struct super_block *sb);
int erofs_getxattr(struct inode *, int, const char *, void *, size_t);
ssize_t erofs_listxattr(struct dentry *, char *, size_t);
#else
static inline int erofs_xattr_prefixes_init(struct super_block *sb) { return 0; }
static inline void erofs_xattr_prefixes_cleanup(struct super_block *sb) {}
static inline int erofs_getxattr(struct inode *inode, int index,
				 const char *name, void *buffer,
				 size_t buffer_size)
{
	return -EOPNOTSUPP;
}

#define erofs_listxattr (NULL)
#define erofs_xattr_handlers (NULL)
#endif	/* !CONFIG_EROFS_FS_XATTR */

#endif
