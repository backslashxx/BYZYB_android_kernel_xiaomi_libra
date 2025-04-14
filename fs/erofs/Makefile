# SPDX-License-Identifier: GPL-2.0

ccflags-y="-DCONFIG_EROFS_FS_XATTR"

obj-$(CONFIG_EROFS_FS) += erofs.o
erofs-objs := super.o inode.o data.o namei.o dir.o xattr.o
