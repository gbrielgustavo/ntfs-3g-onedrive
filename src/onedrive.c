/*
 * onedrive.c - Plugin for accessing Windows OneDrive files from NTFS-3G 
 *
 * Copyright (C) 2017-2020 Jean-Pierre Andre
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *			History
 *
 *		Version 1.0.0, Nov 2017
 *	- first version
 *
 *		Version 1.0.1, Dec 2017
 *	- accepted several reparse tags
 *
 *		Version 1.1.0, Oct 2018
 *	- allowed accessing plain files
 *
 *		Version 1.1.1, Jun 2020
 *	- fixed build configuration
 *
 *		Version 1.2.0, Dec 2020
 *	- implemented creating/linking/unlinking files
 */

#define ONEDRIVE_VERSION "1.2.0"

#include "config.h"

/*
 * Although fuse.h is only needed for 'struct fuse_file_info', we still need to
 * request a specific FUSE API version.  (It's required on FreeBSD, and it's
 * probably a good idea to request the same version used by NTFS-3G anyway.)
 */
#define FUSE_USE_VERSION 26
#include <fuse.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/index.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/plugin.h>
#include <ntfs-3g/misc.h>

struct ONEDRIVE_REPARSE {
	le32 reparse_tag;		/* Reparse point type (inc. flags). */
	le16 reparse_data_length;	/* Byte size of reparse data. */
	le16 reserved;			/* Align to 8-byte boundary. */
	le32 unknown[2];
	GUID guid;
	le16 namelen;			/* Count of ntfschars in name */
	ntfschar name[1];		/* Optional name (variable length) */
} ;

/*
 *		Get the size and mode of a onedrive directory
 */

static int onedrive_getattr(ntfs_inode *ni, const REPARSE_POINT *reparse,
			      struct stat *stbuf)
{
	static ntfschar I30[] =
		{ const_cpu_to_le16('$'), const_cpu_to_le16('I'),
		  const_cpu_to_le16('3'), const_cpu_to_le16('0') };
	ntfs_attr *na;
	int res;

	res = -EOPNOTSUPP;
	if (ni && reparse && stbuf
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)) {
		if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
				/* Directory */
			stbuf->st_mode = S_IFDIR | 0555;
			/* get index size, if not known */
			if (!test_nino_flag(ni, KnownSize)) {
				na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION,
						I30, 4);
				if (na) {
					ni->data_size = na->data_size;
					ni->allocated_size = na->allocated_size;
					set_nino_flag(ni, KnownSize);
					ntfs_attr_close(na);
				}
			}
			stbuf->st_size = ni->data_size;
			stbuf->st_blocks = ni->allocated_size >> 9;
			stbuf->st_nlink = 1;	/* Make find(1) work */
			res = 0;
		} else {
			/* File */
			stbuf->st_size = ni->data_size;
			stbuf->st_blocks = (ni->data_size + 511) >> 9;
			stbuf->st_mode = S_IFREG | 0555;
			res = 0;
		}
	}
	/* Not a onedrive file/directory, or some other error occurred */
	return (res);
}

/*
 *		Open a onedrive directory for reading
 *
 *	Currently no reading context is created.
 */

static int onedrive_opendir(ntfs_inode *ni __attribute__((unused)),
			   const REPARSE_POINT *reparse,
			   struct fuse_file_info *fi)
{
	int res;

	res = -EOPNOTSUPP;
	if (ni && reparse && fi
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)
	    && (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
	    && ((fi->flags & O_ACCMODE) == O_RDONLY))
		res = 0;
	return (res);
}

/*
 *		Release a onedrive directory
 *
 *	Should never be called, as we did not define a reading context
 */

static int onedrive_release(ntfs_inode *ni __attribute__((unused)),
			   const REPARSE_POINT *reparse __attribute__((unused)),
			   struct fuse_file_info *fi __attribute__((unused)))
{
	return 0;
}

/*
 *		Open a onedrive file
 *
 *	Currently no reading context is created.
 */

static int onedrive_open(ntfs_inode *ni, const REPARSE_POINT *reparse,
			   struct fuse_file_info *fi __attribute__((unused)))
{
	int res;

	res = -EOPNOTSUPP;
	if (ni && reparse
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)
	    && !(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		if (ni->flags & FILE_ATTR_OFFLINE)
			res = -EREMOTE; /* No local data */
		else
			res = 0;
	}
	return (res);
}

/*
 *		Create a new file or directory in an onedrive directory
 *	The file is created with no onedrive attribute. It is expected
 *	to be synchronized subsequently by Windows in mode "always keep
 *	on device" (check needed).
 */

static ntfs_inode *onedrive_create(ntfs_inode *dir_ni,
			const REPARSE_POINT *reparse, le32 securid,
			ntfschar *name, int name_len, mode_t type)
{
	ntfs_inode *ni;

	if (dir_ni && reparse
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)
	    && (dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
	    && ((type == S_IFREG) || (type == S_IFDIR))) {
		ni = ntfs_create(dir_ni, securid, name, name_len, type);
	} else {
		ni = (ntfs_inode*)NULL;
		errno = EOPNOTSUPP;
	}
	return (ni);
}

/*
 *		Link a new name to an onedrive file or directory
 */

static int onedrive_link(ntfs_inode *dir_ni, const REPARSE_POINT *reparse,
			ntfs_inode *ni, ntfschar *name, int name_len)
{
	int res;

	if (dir_ni && reparse
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)
	    && (dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		res = ntfs_link(ni, dir_ni, name, name_len);
	} else {
		res = -EOPNOTSUPP;
	}
	return (res);
}

/*
 *		Unlink a name from a directory
 */

static int onedrive_unlink(ntfs_inode *dir_ni, const REPARSE_POINT *reparse,
			const char *pathname,
			ntfs_inode *ni, ntfschar *name, int name_len)
{
	int res;

	if (dir_ni && reparse
	    && !((reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
		& IO_REPARSE_PLUGIN_SELECT)
	    && (dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		res = ntfs_delete(dir_ni->vol, pathname, ni,
				dir_ni, name, name_len);
	} else {
		res = -EOPNOTSUPP;
	}
	return (res);
}

/*
 *		Read an open file
 *
 *	Returns the count of bytes read or a negative error code.
 */

static int onedrive_read(ntfs_inode *ni, const REPARSE_POINT *reparse,
			   char *buf, size_t size, off_t offset,
			   struct fuse_file_info *fi __attribute__((unused)))
{
	const struct ONEDRIVE_REPARSE *onedrive_reparse;
	ntfs_attr *na = NULL;
	s64 total = 0;
	s64 max_read;
	int res;

	res = -EOPNOTSUPP;
	onedrive_reparse = (struct ONEDRIVE_REPARSE*)reparse;
	if (ni && reparse && buf
	    && !((onedrive_reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
			& IO_REPARSE_PLUGIN_SELECT)) {
		na = ntfs_attr_open(ni, AT_DATA, (ntfschar*)NULL, 0);
		if (!na) {
			res = -errno;
			goto exit;
		}
		max_read = na->data_size;
		if (offset + (off_t)size > max_read) {
			if (max_read < offset)
				goto ok;
			size = max_read - offset;
		}
		while (size > 0) {
			s64 ret = ntfs_attr_pread(na, offset, size,
					buf + total);
			if (ret != (s64)size) {
				ntfs_log_perror("onedrive_read error reading inode "
					"%lld at offset %lld\n",
					(long long)ni->mft_no,
					(long long)offset);
				if (ret <= 0 || ret > (s64)size) {
					ntfs_attr_close(na);
					res = (ret < 0) ? -errno : -EIO;
					goto exit;
				}
			}
			size -= ret;
			offset += ret;
			total += ret;
		}
ok:
		ntfs_attr_close(na);
		res = total;
	} else {
		res = -EINVAL;
	}
exit :
	return (res);
}

/*
 *		Write to an open file
 *
 *	Returns the count of bytes written or a negative error code.
 */

static int onedrive_write(ntfs_inode *ni, const REPARSE_POINT *reparse,
			   const char *buf, size_t size, off_t offset,
			   struct fuse_file_info *fi __attribute__((unused)))
{
	const struct ONEDRIVE_REPARSE *onedrive_reparse;
	ntfs_attr *na = NULL;
	s64 total = 0;
	int res;

	res = -EOPNOTSUPP;
	onedrive_reparse = (struct ONEDRIVE_REPARSE*)reparse;
	if (ni && reparse && buf
	    && !(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
	    && !((onedrive_reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
			& IO_REPARSE_PLUGIN_SELECT)) {
		na = ntfs_attr_open(ni, AT_DATA, (ntfschar*)NULL, 0);
		if (!na) {
			res = -errno;
			goto exit;
		}
		while (size > 0) {
			s64 ret = ntfs_attr_pwrite(na, offset, size,
						buf + total);
			if (ret <= 0) {
				ntfs_log_perror("onedrive_write error writing"
					" to inode %lld at offset %lld\n",
					(long long)ni->mft_no,
					(long long)offset);
				ntfs_attr_close(na);
				res = (ret < 0) ? -errno : -EIO;
				goto exit;
			}
			size -= ret;
			offset += ret;
			total += ret;
		}
		ntfs_attr_close(na);
		res = total;
	} else {
		res = -EINVAL;
	}
exit :
	return (res);
}

/*
 *		Truncate an open file
 *
 *	Returns zero or a negative error code.
 */

static int onedrive_truncate(ntfs_inode *ni, const REPARSE_POINT *reparse,
			   off_t size)
{
	const struct ONEDRIVE_REPARSE *onedrive_reparse;
	ntfs_attr *na = NULL;
	int res;

	res = -EOPNOTSUPP;
	onedrive_reparse = (struct ONEDRIVE_REPARSE*)reparse;
	if (ni && reparse
	    && !(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
	    && !((onedrive_reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
			& IO_REPARSE_PLUGIN_SELECT)) {
		na = ntfs_attr_open(ni, AT_DATA, (ntfschar*)NULL, 0);
		if (!na) {
			res = -errno;
			goto exit;
		}
		res = ntfs_attr_truncate(na, size);
		ntfs_attr_close(na);
	} else {
		res = -EINVAL;
	}
exit :
	return (res);
}

/*
 *		Read an open directory
 *
 *	Returns 0 or a negative error code
 */

static int onedrive_readdir(ntfs_inode *ni, const REPARSE_POINT *reparse,
			s64 *pos, void *fillctx, ntfs_filldir_t filldir,
			struct fuse_file_info *fi __attribute__((unused)))
{
	const struct ONEDRIVE_REPARSE *onedrive_reparse;
	int res;

	res = -EOPNOTSUPP;
	onedrive_reparse = (struct ONEDRIVE_REPARSE*)reparse;
	if (ni && reparse && pos && fillctx && filldir
	    && (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)
	    && !((onedrive_reparse->reparse_tag ^ IO_REPARSE_TAG_CLOUD)
			& IO_REPARSE_PLUGIN_SELECT)) {
		res = 0;
		if (ntfs_readdir(ni, pos, fillctx, filldir))
			res = -errno;
	}
	return (res);
}

static const struct plugin_operations ops = {
	.getattr = onedrive_getattr,
	.open = onedrive_open,
	.read = onedrive_read,
	.write = onedrive_write,
	.truncate = onedrive_truncate,
	.release = onedrive_release,
	.opendir = onedrive_opendir,
	.readdir = onedrive_readdir,
	.create = onedrive_create,
	.link = onedrive_link,
	.unlink = onedrive_unlink,
};

/*
 *		Initialize the plugin and return its methods.
 */

const struct plugin_operations *init(le32 tag)
{
	const struct plugin_operations *pops;

	pops = (const struct plugin_operations*)NULL;
	if (!((tag ^ IO_REPARSE_TAG_CLOUD) & IO_REPARSE_PLUGIN_SELECT)) {
		pops = &ops;
	} else {
		ntfs_log_error("Error in OneDrive plugin call\n");
	}
	if (pops)
		ntfs_log_info("OneDrive plugin %s for ntfs-3g\n",
					ONEDRIVE_VERSION);
	else
		errno = EINVAL;
	return (pops);
}
