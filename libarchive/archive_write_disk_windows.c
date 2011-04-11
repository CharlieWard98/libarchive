/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * Copyright (c) 2011 Michihiro NAKAJIMA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"
__FBSDID("$FreeBSD$");

#if defined(_WIN32) && !defined(__CYGWIN__)

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <winioctl.h>

/* TODO: Support Mac OS 'quarantine' feature.  This is really just a
 * standard tag to mark files that have been downloaded as "tainted".
 * On Mac OS, we should mark the extracted files as tainted if the
 * archive being read was tainted.  Windows has a similar feature; we
 * should investigate ways to support this generically. */

#include "archive.h"
#include "archive_acl_private.h"
#include "archive_string.h"
#include "archive_entry.h"
#include "archive_private.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct fixup_entry {
	struct fixup_entry	*next;
	struct archive_acl	 acl;
	mode_t			 mode;
	int64_t			 atime;
	int64_t                  birthtime;
	int64_t			 mtime;
	int64_t			 ctime;
	unsigned long		 atime_nanos;
	unsigned long            birthtime_nanos;
	unsigned long		 mtime_nanos;
	unsigned long		 ctime_nanos;
	unsigned long		 fflags_set;
	size_t			 mac_metadata_size;
	void			*mac_metadata;
	int			 fixup; /* bitmask of what needs fixing */
	char			*name;
};

/*
 * We use a bitmask to track which operations remain to be done for
 * this file.  In particular, this helps us avoid unnecessary
 * operations when it's possible to take care of one step as a
 * side-effect of another.  For example, mkdir() can specify the mode
 * for the newly-created object but symlink() cannot.  This means we
 * can skip chmod() if mkdir() succeeded, but we must explicitly
 * chmod() if we're trying to create a directory that already exists
 * (mkdir() failed) or if we're restoring a symlink.  Similarly, we
 * need to verify UID/GID before trying to restore SUID/SGID bits;
 * that verification can occur explicitly through a stat() call or
 * implicitly because of a successful chown() call.
 */
#define	TODO_MODE_FORCE		0x40000000
#define	TODO_MODE_BASE		0x20000000
#define	TODO_SUID		0x10000000
#define	TODO_SUID_CHECK		0x08000000
#define	TODO_SGID		0x04000000
#define	TODO_SGID_CHECK		0x02000000
#define	TODO_MODE		(TODO_MODE_BASE|TODO_SUID|TODO_SGID)
#define	TODO_TIMES		ARCHIVE_EXTRACT_TIME
#define	TODO_OWNER		ARCHIVE_EXTRACT_OWNER
#define	TODO_FFLAGS		ARCHIVE_EXTRACT_FFLAGS
#define	TODO_ACLS		ARCHIVE_EXTRACT_ACL
#define	TODO_XATTR		ARCHIVE_EXTRACT_XATTR
#define	TODO_MAC_METADATA	ARCHIVE_EXTRACT_MAC_METADATA

struct archive_write_disk {
	struct archive	archive;

	mode_t			 user_umask;
	struct fixup_entry	*fixup_list;
	struct fixup_entry	*current_fixup;
	int64_t			 user_uid;
	dev_t			 skip_file_dev;
	ino_t			 skip_file_ino;
	time_t			 start_time;

	int64_t (*lookup_gid)(void *private, const char *gname, int64_t gid);
	void  (*cleanup_gid)(void *private);
	void			*lookup_gid_data;
	int64_t (*lookup_uid)(void *private, const char *uname, int64_t uid);
	void  (*cleanup_uid)(void *private);
	void			*lookup_uid_data;

	/*
	 * Full path of last file to satisfy symlink checks.
	 */
	struct archive_string	path_safe;

	/*
	 * Cached stat data from disk for the current entry.
	 * If this is valid, pst points to st.  Otherwise,
	 * pst is null.
	 */
	struct stat		 st;
	struct stat		*pst;

	/* Information about the object being restored right now. */
	struct archive_entry	*entry; /* Entry being extracted. */
	char			*name; /* Name of entry, possibly edited. */
	struct archive_string	 _name_data; /* backing store for 'name' */
	/* Tasks remaining for this object. */
	int			 todo;
	/* Tasks deferred until end-of-archive. */
	int			 deferred;
	/* Options requested by the client. */
	int			 flags;
	/* Handle for the file we're restoring. */
	int			 fd;
	/* Current offset for writing data to the file. */
	int64_t			 offset;
	/* Last offset actually written to disk. */
	int64_t			 fd_offset;
	/* Total bytes actually written to files. */
	int64_t			 total_bytes_written;
	/* Maximum size of file, -1 if unknown. */
	int64_t			 filesize;
	/* Dir we were in before this restore; only for deep paths. */
	int			 restore_pwd;
	/* Mode we should use for this entry; affected by _PERM and umask. */
	mode_t			 mode;
	/* UID/GID to use in restoring this entry. */
	int64_t			 uid;
	int64_t			 gid;
};

/*
 * Default mode for dirs created automatically (will be modified by umask).
 * Note that POSIX specifies 0777 for implicity-created dirs, "modified
 * by the process' file creation mask."
 */
#define	DEFAULT_DIR_MODE 0777
/*
 * Dir modes are restored in two steps:  During the extraction, the permissions
 * in the archive are modified to match the following limits.  During
 * the post-extract fixup pass, the permissions from the archive are
 * applied.
 */
#define	MINIMUM_DIR_MODE 0700
#define	MAXIMUM_DIR_MODE 0775

static int	check_symlinks(struct archive_write_disk *);
static int	create_filesystem_object(struct archive_write_disk *);
static struct fixup_entry *current_fixup(struct archive_write_disk *, const char *pathname);
#if defined(HAVE_FCHDIR) && defined(PATH_MAX)
static void	edit_deep_directories(struct archive_write_disk *ad);
#endif
static int	cleanup_pathname(struct archive_write_disk *);
static int	create_dir(struct archive_write_disk *, char *);
static int	create_parent_dir(struct archive_write_disk *, char *);
static int	older(struct stat *, struct archive_entry *);
static int	restore_entry(struct archive_write_disk *);
#ifdef HAVE_POSIX_ACL
static int	set_acl(struct archive_write_disk *, int fd, const char *, struct archive_acl *,
		    acl_type_t, int archive_entry_acl_type, const char *tn);
#endif
static int	set_acls(struct archive_write_disk *, int fd, const char *, struct archive_acl *);
static int	set_xattrs(struct archive_write_disk *);
static int	set_fflags(struct archive_write_disk *);
static int	set_ownership(struct archive_write_disk *);
static int	set_mode(struct archive_write_disk *, int mode);
static int	set_times(struct archive_write_disk *, int, int, const char *,
		    time_t, long, time_t, long, time_t, long, time_t, long);
static int	set_times_from_entry(struct archive_write_disk *);
static struct fixup_entry *sort_dir_list(struct fixup_entry *p);
static int64_t	trivial_lookup_gid(void *, const char *, int64_t);
static int64_t	trivial_lookup_uid(void *, const char *, int64_t);
static ssize_t	write_data_block(struct archive_write_disk *,
		    const char *, size_t);

static struct archive_vtable *archive_write_disk_vtable(void);

static int	_archive_write_disk_close(struct archive *);
static int	_archive_write_disk_free(struct archive *);
static int	_archive_write_disk_header(struct archive *, struct archive_entry *);
static int64_t	_archive_write_disk_filter_bytes(struct archive *, int);
static int	_archive_write_disk_finish_entry(struct archive *);
static ssize_t	_archive_write_disk_data(struct archive *, const void *, size_t);
static ssize_t	_archive_write_disk_data_block(struct archive *, const void *, size_t, int64_t);

static int
lazy_stat(struct archive_write_disk *a)
{
	if (a->pst != NULL) {
		/* Already have stat() data available. */
		return (ARCHIVE_OK);
	}
#ifdef HAVE_FSTAT
	if (a->fd >= 0 && fstat(a->fd, &a->st) == 0) {
		a->pst = &a->st;
		return (ARCHIVE_OK);
	}
#endif
	/*
	 * XXX At this point, symlinks should not be hit, otherwise
	 * XXX a race occured.  Do we want to check explicitly for that?
	 */
	if (lstat(a->name, &a->st) == 0) {
		a->pst = &a->st;
		return (ARCHIVE_OK);
	}
	archive_set_error(&a->archive, errno, "Couldn't stat file");
	return (ARCHIVE_WARN);
}

static struct archive_vtable *
archive_write_disk_vtable(void)
{
	static struct archive_vtable av;
	static int inited = 0;

	if (!inited) {
		av.archive_close = _archive_write_disk_close;
		av.archive_filter_bytes = _archive_write_disk_filter_bytes;
		av.archive_free = _archive_write_disk_free;
		av.archive_write_header = _archive_write_disk_header;
		av.archive_write_finish_entry
		    = _archive_write_disk_finish_entry;
		av.archive_write_data = _archive_write_disk_data;
		av.archive_write_data_block = _archive_write_disk_data_block;
		inited = 1;
	}
	return (&av);
}

static int64_t
_archive_write_disk_filter_bytes(struct archive *_a, int n)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	(void)n; /* UNUSED */
	if (n == -1 || n == 0)
		return (a->total_bytes_written);
	return (-1);
}


int
archive_write_disk_set_options(struct archive *_a, int flags)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;

	a->flags = flags;
	return (ARCHIVE_OK);
}


/*
 * Extract this entry to disk.
 *
 * TODO: Validate hardlinks.  According to the standards, we're
 * supposed to check each extracted hardlink and squawk if it refers
 * to a file that we didn't restore.  I'm not entirely convinced this
 * is a good idea, but more importantly: Is there any way to validate
 * hardlinks without keeping a complete list of filenames from the
 * entire archive?? Ugh.
 *
 */
static int
_archive_write_disk_header(struct archive *_a, struct archive_entry *entry)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	struct fixup_entry *fe;
	int ret, r;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_write_disk_header");
	archive_clear_error(&a->archive);
	if (a->archive.state & ARCHIVE_STATE_DATA) {
		r = _archive_write_disk_finish_entry(&a->archive);
		if (r == ARCHIVE_FATAL)
			return (r);
	}

	/* Set up for this particular entry. */
	a->pst = NULL;
	a->current_fixup = NULL;
	a->deferred = 0;
	if (a->entry) {
		archive_entry_free(a->entry);
		a->entry = NULL;
	}
	a->entry = archive_entry_clone(entry);
	a->fd = -1;
	a->fd_offset = 0;
	a->offset = 0;
	a->restore_pwd = -1;
	a->uid = a->user_uid;
	a->mode = archive_entry_mode(a->entry);
	if (archive_entry_size_is_set(a->entry))
		a->filesize = archive_entry_size(a->entry);
	else
		a->filesize = -1;
	archive_strcpy(&(a->_name_data), archive_entry_pathname(a->entry));
	a->name = a->_name_data.s;
	archive_clear_error(&a->archive);

	/*
	 * Clean up the requested path.  This is necessary for correct
	 * dir restores; the dir restore logic otherwise gets messed
	 * up by nonsense like "dir/.".
	 */
	ret = cleanup_pathname(a);
	if (ret != ARCHIVE_OK)
		return (ret);

	/*
	 * Query the umask so we get predictable mode settings.
	 * This gets done on every call to _write_header in case the
	 * user edits their umask during the extraction for some
	 * reason.
	 */
	umask(a->user_umask = umask(0));

	/* Figure out what we need to do for this entry. */
	a->todo = TODO_MODE_BASE;
	if (a->flags & ARCHIVE_EXTRACT_PERM) {
		a->todo |= TODO_MODE_FORCE; /* Be pushy about permissions. */
		/*
		 * SGID requires an extra "check" step because we
		 * cannot easily predict the GID that the system will
		 * assign.  (Different systems assign GIDs to files
		 * based on a variety of criteria, including process
		 * credentials and the gid of the enclosing
		 * directory.)  We can only restore the SGID bit if
		 * the file has the right GID, and we only know the
		 * GID if we either set it (see set_ownership) or if
		 * we've actually called stat() on the file after it
		 * was restored.  Since there are several places at
		 * which we might verify the GID, we need a TODO bit
		 * to keep track.
		 */
		if (a->mode & S_ISGID)
			a->todo |= TODO_SGID | TODO_SGID_CHECK;
		/*
		 * Verifying the SUID is simpler, but can still be
		 * done in multiple ways, hence the separate "check" bit.
		 */
		if (a->mode & S_ISUID)
			a->todo |= TODO_SUID | TODO_SUID_CHECK;
	} else {
		/*
		 * User didn't request full permissions, so don't
		 * restore SUID, SGID bits and obey umask.
		 */
		a->mode &= ~S_ISUID;
		a->mode &= ~S_ISGID;
		a->mode &= ~S_ISVTX;
		a->mode &= ~a->user_umask;
	}
#if 0
	if (a->flags & ARCHIVE_EXTRACT_OWNER)
		a->todo |= TODO_OWNER;
#endif
	if (a->flags & ARCHIVE_EXTRACT_TIME)
		a->todo |= TODO_TIMES;
	if (a->flags & ARCHIVE_EXTRACT_ACL) {
		if (archive_entry_filetype(a->entry) == AE_IFDIR)
			a->deferred |= TODO_ACLS;
		else
			a->todo |= TODO_ACLS;
	}
	if (a->flags & ARCHIVE_EXTRACT_MAC_METADATA) {
		if (archive_entry_filetype(a->entry) == AE_IFDIR)
			a->deferred |= TODO_MAC_METADATA;
		else
			a->todo |= TODO_MAC_METADATA;
	}
	if (a->flags & ARCHIVE_EXTRACT_XATTR)
		a->todo |= TODO_XATTR;
	if (a->flags & ARCHIVE_EXTRACT_FFLAGS)
		a->todo |= TODO_FFLAGS;
	if (a->flags & ARCHIVE_EXTRACT_SECURE_SYMLINKS) {
		ret = check_symlinks(a);
		if (ret != ARCHIVE_OK)
			return (ret);
	}

	ret = restore_entry(a);

	/*
	 * TODO: There are rumours that some extended attributes must
	 * be restored before file data is written.  If this is true,
	 * then we either need to write all extended attributes both
	 * before and after restoring the data, or find some rule for
	 * determining which must go first and which last.  Due to the
	 * many ways people are using xattrs, this may prove to be an
	 * intractable problem.
	 */

	/*
	 * Fixup uses the unedited pathname from archive_entry_pathname(),
	 * because it is relative to the base dir and the edited path
	 * might be relative to some intermediate dir as a result of the
	 * deep restore logic.
	 */
	if (a->deferred & TODO_MODE) {
		fe = current_fixup(a, archive_entry_pathname(entry));
		fe->fixup |= TODO_MODE_BASE;
		fe->mode = a->mode;
	}

	if ((a->deferred & TODO_TIMES)
		&& (archive_entry_mtime_is_set(entry)
		    || archive_entry_atime_is_set(entry))) {
		fe = current_fixup(a, archive_entry_pathname(entry));
		fe->mode = a->mode;
		fe->fixup |= TODO_TIMES;
		if (archive_entry_atime_is_set(entry)) {
			fe->atime = archive_entry_atime(entry);
			fe->atime_nanos = archive_entry_atime_nsec(entry);
		} else {
			/* If atime is unset, use start time. */
			fe->atime = a->start_time;
			fe->atime_nanos = 0;
		}
		if (archive_entry_mtime_is_set(entry)) {
			fe->mtime = archive_entry_mtime(entry);
			fe->mtime_nanos = archive_entry_mtime_nsec(entry);
		} else {
			/* If mtime is unset, use start time. */
			fe->mtime = a->start_time;
			fe->mtime_nanos = 0;
		}
		if (archive_entry_birthtime_is_set(entry)) {
			fe->birthtime = archive_entry_birthtime(entry);
			fe->birthtime_nanos = archive_entry_birthtime_nsec(entry);
		} else {
			/* If birthtime is unset, use mtime. */
			fe->birthtime = fe->mtime;
			fe->birthtime_nanos = fe->mtime_nanos;
		}
	}

	if (a->deferred & TODO_ACLS) {
		fe = current_fixup(a, archive_entry_pathname(entry));
		archive_acl_copy(&fe->acl, archive_entry_acl(entry));
	}

	if (a->deferred & TODO_MAC_METADATA) {
		const void *metadata;
		size_t metadata_size;
		metadata = archive_entry_mac_metadata(a->entry, &metadata_size);
		if (metadata != NULL && metadata_size > 0) {
			fe = current_fixup(a, archive_entry_pathname(entry));
			fe->mac_metadata = malloc(metadata_size);
			if (fe->mac_metadata != NULL) {
				memcpy(fe->mac_metadata, metadata, metadata_size);
				fe->mac_metadata_size = metadata_size;
				fe->fixup |= TODO_MAC_METADATA;
			}
		}
	}

	if (a->deferred & TODO_FFLAGS) {
		fe = current_fixup(a, archive_entry_pathname(entry));
		fe->fixup |= TODO_FFLAGS;
		/* TODO: Complete this.. defer fflags from below. */
	}

	/*
	 * On Windows, A creating sparse file requires a special mark.
	 */
	if (a->fd >= 0 && archive_entry_sparse_count(entry) > 0) {
		int64_t base = 0, offset, length;
		int i, cnt = archive_entry_sparse_reset(entry);
		int sparse = 0;

		for (i = 0; i < cnt; i++) {
			archive_entry_sparse_next(entry, &offset, &length);
			if (offset - base >= 4096) {
				sparse = 1;/* we have a hole. */
				break;
			}
			base = offset + length;
		}
		if (sparse) {
			HANDLE h = (HANDLE)_get_osfhandle(a->fd);
			DWORD dmy;
			/* Mark this file as sparse. */
			DeviceIoControl(h, FSCTL_SET_SPARSE,
			    NULL, 0, NULL, 0, &dmy, NULL);
		}
	}

	/* We've created the object and are ready to pour data into it. */
	if (ret >= ARCHIVE_WARN)
		a->archive.state = ARCHIVE_STATE_DATA;
	/*
	 * If it's not open, tell our client not to try writing.
	 * In particular, dirs, links, etc, don't get written to.
	 */
	if (a->fd < 0) {
		archive_entry_set_size(entry, 0);
		a->filesize = 0;
	}

	return (ret);
}

int
archive_write_disk_set_skip_file(struct archive *_a, int64_t d, int64_t i)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_disk_set_skip_file");
	a->skip_file_dev = d;
	a->skip_file_ino = i;
	return (ARCHIVE_OK);
}

static ssize_t
write_data_block(struct archive_write_disk *a, const char *buff, size_t size)
{
	uint64_t start_size = size;
	ssize_t bytes_written = 0;
	ssize_t block_size = 0, bytes_to_write;

	if (size == 0)
		return (ARCHIVE_OK);

	if (a->filesize == 0 || a->fd < 0) {
		archive_set_error(&a->archive, 0,
		    "Attempt to write to an empty file");
		return (ARCHIVE_WARN);
	}

	if (a->flags & ARCHIVE_EXTRACT_SPARSE) {
		/* XXX TODO XXX Is there a more appropriate choice here ? */
		/* This needn't match the filesystem allocation size. */
		block_size = 16*1024;
	}

	/* If this write would run beyond the file size, truncate it. */
	if (a->filesize >= 0 && (int64_t)(a->offset + size) > a->filesize)
		start_size = size = (size_t)(a->filesize - a->offset);

	/* Write the data. */
	while (size > 0) {
		if (block_size == 0) {
			bytes_to_write = size;
		} else {
			/* We're sparsifying the file. */
			const char *p, *end;
			int64_t block_end;

			/* Skip leading zero bytes. */
			for (p = buff, end = buff + size; p < end; ++p) {
				if (*p != '\0')
					break;
			}
			a->offset += p - buff;
			size -= p - buff;
			buff = p;
			if (size == 0)
				break;

			/* Calculate next block boundary after offset. */
			block_end
			    = (a->offset / block_size + 1) * block_size;

			/* If the adjusted write would cross block boundary,
			 * truncate it to the block boundary. */
			bytes_to_write = size;
			if (a->offset + bytes_to_write > block_end)
				bytes_to_write = block_end - a->offset;
		}
		/* Seek if necessary to the specified offset. */
		if (a->offset != a->fd_offset) {
			if (lseek(a->fd, a->offset, SEEK_SET) < 0) {
				archive_set_error(&a->archive, errno,
				    "Seek failed");
				return (ARCHIVE_FATAL);
			}
			a->fd_offset = a->offset;
 		}
		bytes_written = write(a->fd, buff, bytes_to_write);
		if (bytes_written < 0) {
			archive_set_error(&a->archive, errno, "Write failed");
			return (ARCHIVE_WARN);
		}
		buff += bytes_written;
		size -= bytes_written;
		a->total_bytes_written += bytes_written;
		a->offset += bytes_written;
		a->fd_offset = a->offset;
	}
	return (start_size - size);
}

static ssize_t
_archive_write_disk_data_block(struct archive *_a,
    const void *buff, size_t size, int64_t offset)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	ssize_t r;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_DATA, "archive_write_data_block");

	a->offset = offset;
	r = write_data_block(a, buff, size);
	if (r < ARCHIVE_OK)
		return (r);
	if ((size_t)r < size) {
		archive_set_error(&a->archive, 0,
		    "Write request too large");
		return (ARCHIVE_WARN);
	}
	return (ARCHIVE_OK);
}

static ssize_t
_archive_write_disk_data(struct archive *_a, const void *buff, size_t size)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_DATA, "archive_write_data");

	return (write_data_block(a, buff, size));
}

static int
_archive_write_disk_finish_entry(struct archive *_a)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	int ret = ARCHIVE_OK;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_write_finish_entry");
	if (a->archive.state & ARCHIVE_STATE_HEADER)
		return (ARCHIVE_OK);
	archive_clear_error(&a->archive);

	/* Pad or truncate file to the right size. */
	if (a->fd < 0) {
		/* There's no file. */
	} else if (a->filesize < 0) {
		/* File size is unknown, so we can't set the size. */
	} else if (a->fd_offset == a->filesize) {
		/* Last write ended at exactly the filesize; we're done. */
		/* Hopefully, this is the common case. */
	} else {
#if HAVE_FTRUNCATE
		if (ftruncate(a->fd, a->filesize) == -1 &&
		    a->filesize == 0) {
			archive_set_error(&a->archive, errno,
			    "File size could not be restored");
			return (ARCHIVE_FAILED);
		}
#endif
		/*
		 * Not all platforms implement the XSI option to
		 * extend files via ftruncate.  Stat() the file again
		 * to see what happened.
		 */
		a->pst = NULL;
		if ((ret = lazy_stat(a)) != ARCHIVE_OK)
			return (ret);
		/* We can use lseek()/write() to extend the file if
		 * ftruncate didn't work or isn't available. */
		if (a->st.st_size < a->filesize) {
			const char nul = '\0';
			if (lseek(a->fd, a->filesize - 1, SEEK_SET) < 0) {
				archive_set_error(&a->archive, errno,
				    "Seek failed");
				return (ARCHIVE_FATAL);
			}
			if (write(a->fd, &nul, 1) < 0) {
				archive_set_error(&a->archive, errno,
				    "Write to restore size failed");
				return (ARCHIVE_FATAL);
			}
			a->pst = NULL;
		}
	}

	/* Restore metadata. */

	/*
	 * Look up the "real" UID only if we're going to need it.
	 * TODO: the TODO_SGID condition can be dropped here, can't it?
	 */
	if (a->todo & (TODO_OWNER | TODO_SUID | TODO_SGID)) {
		a->uid = a->lookup_uid(a->lookup_uid_data,
		    archive_entry_uname(a->entry),
		    archive_entry_uid(a->entry));
	}
	/* Look up the "real" GID only if we're going to need it. */
	/* TODO: the TODO_SUID condition can be dropped here, can't it? */
	if (a->todo & (TODO_OWNER | TODO_SGID | TODO_SUID)) {
		a->gid = a->lookup_gid(a->lookup_gid_data,
		    archive_entry_gname(a->entry),
		    archive_entry_gid(a->entry));
	 }

	/*
	 * Restore ownership before set_mode tries to restore suid/sgid
	 * bits.  If we set the owner, we know what it is and can skip
	 * a stat() call to examine the ownership of the file on disk.
	 */
	if (a->todo & TODO_OWNER)
		ret = set_ownership(a);

	/*
	 * set_mode must precede ACLs on systems such as Solaris and
	 * FreeBSD where setting the mode implicitly clears extended ACLs
	 */
	if (a->todo & TODO_MODE) {
		int r2 = set_mode(a, a->mode);
		if (r2 < ret) ret = r2;
	}

	/*
	 * Security-related extended attributes (such as
	 * security.capability on Linux) have to be restored last,
	 * since they're implicitly removed by other file changes.
	 */
	if (a->todo & TODO_XATTR) {
		int r2 = set_xattrs(a);
		if (r2 < ret) ret = r2;
	}

	/*
	 * Some flags prevent file modification; they must be restored after
	 * file contents are written.
	 */
	if (a->todo & TODO_FFLAGS) {
		int r2 = set_fflags(a);
		if (r2 < ret) ret = r2;
	}

	/*
	 * Time must follow most other metadata;
	 * otherwise atime will get changed.
	 */
	if (a->todo & TODO_TIMES) {
		int r2 = set_times_from_entry(a);
		if (r2 < ret) ret = r2;
	}

	/*
	 * ACLs must be restored after timestamps because there are
	 * ACLs that prevent attribute changes (including time).
	 */
	if (a->todo & TODO_ACLS) {
		int r2 = set_acls(a, a->fd,
				  archive_entry_pathname(a->entry),
				  archive_entry_acl(a->entry));
		if (r2 < ret) ret = r2;
	}

	/* If there's an fd, we can close it now. */
	if (a->fd >= 0) {
		close(a->fd);
		a->fd = -1;
	}
	/* If there's an entry, we can release it now. */
	if (a->entry) {
		archive_entry_free(a->entry);
		a->entry = NULL;
	}
	a->archive.state = ARCHIVE_STATE_HEADER;
	return (ret);
}

int
archive_write_disk_set_group_lookup(struct archive *_a,
    void *private_data,
    int64_t (*lookup_gid)(void *private, const char *gname, int64_t gid),
    void (*cleanup_gid)(void *private))
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_disk_set_group_lookup");

	a->lookup_gid = lookup_gid;
	a->cleanup_gid = cleanup_gid;
	a->lookup_gid_data = private_data;
	return (ARCHIVE_OK);
}

int
archive_write_disk_set_user_lookup(struct archive *_a,
    void *private_data,
    int64_t (*lookup_uid)(void *private, const char *uname, int64_t uid),
    void (*cleanup_uid)(void *private))
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_write_disk_set_user_lookup");

	a->lookup_uid = lookup_uid;
	a->cleanup_uid = cleanup_uid;
	a->lookup_uid_data = private_data;
	return (ARCHIVE_OK);
}


/*
 * Create a new archive_write_disk object and initialize it with global state.
 */
struct archive *
archive_write_disk_new(void)
{
	struct archive_write_disk *a;

	a = (struct archive_write_disk *)malloc(sizeof(*a));
	if (a == NULL)
		return (NULL);
	memset(a, 0, sizeof(*a));
	a->archive.magic = ARCHIVE_WRITE_DISK_MAGIC;
	/* We're ready to write a header immediately. */
	a->archive.state = ARCHIVE_STATE_HEADER;
	a->archive.vtable = archive_write_disk_vtable();
	a->lookup_uid = trivial_lookup_uid;
	a->lookup_gid = trivial_lookup_gid;
	a->start_time = time(NULL);
	/* Query and restore the umask. */
	umask(a->user_umask = umask(0));
	if (archive_string_ensure(&a->path_safe, 512) == NULL) {
		free(a);
		return (NULL);
	}
	return (&a->archive);
}


/*
 * The main restore function.
 */
static int
restore_entry(struct archive_write_disk *a)
{
	int ret = ARCHIVE_OK, en;

	if (a->flags & ARCHIVE_EXTRACT_UNLINK && !S_ISDIR(a->mode)) {
		/*
		 * TODO: Fix this.  Apparently, there are platforms
		 * that still allow root to hose the entire filesystem
		 * by unlinking a dir.  The S_ISDIR() test above
		 * prevents us from using unlink() here if the new
		 * object is a dir, but that doesn't mean the old
		 * object isn't a dir.
		 */
		if (unlink(a->name) == 0) {
			/* We removed it, reset cached stat. */
			a->pst = NULL;
		} else if (errno == ENOENT) {
			/* File didn't exist, that's just as good. */
		} else if (rmdir(a->name) == 0) {
			/* It was a dir, but now it's gone. */
			a->pst = NULL;
		} else {
			/* We tried, but couldn't get rid of it. */
			archive_set_error(&a->archive, errno,
			    "Could not unlink");
			return(ARCHIVE_FAILED);
		}
	}

	/* Try creating it first; if this fails, we'll try to recover. */
	en = create_filesystem_object(a);

	if ((en == ENOTDIR || en == ENOENT)
	    && !(a->flags & ARCHIVE_EXTRACT_NO_AUTODIR)) {
		/* If the parent dir doesn't exist, try creating it. */
		create_parent_dir(a, a->name);
		/* Now try to create the object again. */
		en = create_filesystem_object(a);
	}

	if ((en == EISDIR || en == EEXIST)
	    && (a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE)) {
		/* If we're not overwriting, we're done. */
		archive_entry_unset_size(a->entry);
		return (ARCHIVE_OK);
	}

	/*
	 * Some platforms return EISDIR if you call
	 * open(O_WRONLY | O_EXCL | O_CREAT) on a directory, some
	 * return EEXIST.  POSIX is ambiguous, requiring EISDIR
	 * for open(O_WRONLY) on a dir and EEXIST for open(O_EXCL | O_CREAT)
	 * on an existing item.
	 */
	if (en == EISDIR) {
		/* A dir is in the way of a non-dir, rmdir it. */
		if (rmdir(a->name) != 0) {
			archive_set_error(&a->archive, errno,
			    "Can't remove already-existing dir");
			return (ARCHIVE_FAILED);
		}
		a->pst = NULL;
		/* Try again. */
		en = create_filesystem_object(a);
	} else if (en == EEXIST) {
		/*
		 * We know something is in the way, but we don't know what;
		 * we need to find out before we go any further.
		 */
		int r = 0;
		/*
		 * The SECURE_SYMLINK logic has already removed a
		 * symlink to a dir if the client wants that.  So
		 * follow the symlink if we're creating a dir.
		 */
		if (S_ISDIR(a->mode))
			r = stat(a->name, &a->st);
		/*
		 * If it's not a dir (or it's a broken symlink),
		 * then don't follow it.
		 */
		if (r != 0 || !S_ISDIR(a->mode))
			r = lstat(a->name, &a->st);
		if (r != 0) {
			archive_set_error(&a->archive, errno,
			    "Can't stat existing object");
			return (ARCHIVE_FAILED);
		}

		/*
		 * NO_OVERWRITE_NEWER doesn't apply to directories.
		 */
		if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER)
		    &&  !S_ISDIR(a->st.st_mode)) {
			if (!older(&(a->st), a->entry)) {
				archive_entry_unset_size(a->entry);
				return (ARCHIVE_OK);
			}
		}

		/* If it's our archive, we're done. */
		if (a->skip_file_dev > 0 &&
		    a->skip_file_ino > 0 &&
		    a->st.st_dev == a->skip_file_dev &&
		    a->st.st_ino == a->skip_file_ino) {
			archive_set_error(&a->archive, 0, "Refusing to overwrite archive");
			return (ARCHIVE_FAILED);
		}

		if (!S_ISDIR(a->st.st_mode)) {
			/* A non-dir is in the way, unlink it. */
			if (unlink(a->name) != 0) {
				archive_set_error(&a->archive, errno,
				    "Can't unlink already-existing object");
				return (ARCHIVE_FAILED);
			}
			a->pst = NULL;
			/* Try again. */
			en = create_filesystem_object(a);
		} else if (!S_ISDIR(a->mode)) {
			/* A dir is in the way of a non-dir, rmdir it. */
			if (rmdir(a->name) != 0) {
				archive_set_error(&a->archive, errno,
				    "Can't remove already-existing dir");
				return (ARCHIVE_FAILED);
			}
			/* Try again. */
			en = create_filesystem_object(a);
		} else {
			/*
			 * There's a dir in the way of a dir.  Don't
			 * waste time with rmdir()/mkdir(), just fix
			 * up the permissions on the existing dir.
			 * Note that we don't change perms on existing
			 * dirs unless _EXTRACT_PERM is specified.
			 */
			if ((a->mode != a->st.st_mode)
			    && (a->todo & TODO_MODE_FORCE))
				a->deferred |= (a->todo & TODO_MODE);
			/* Ownership doesn't need deferred fixup. */
			en = 0; /* Forget the EEXIST. */
		}
	}

	if (en) {
		/* Everything failed; give up here. */
		archive_set_error(&a->archive, en, "Can't create '%s'",
		    a->name);
		return (ARCHIVE_FAILED);
	}

	a->pst = NULL; /* Cached stat data no longer valid. */
	return (ret);
}

/*
 * Returns 0 if creation succeeds, or else returns errno value from
 * the failed system call.   Note:  This function should only ever perform
 * a single system call.
 */
static int
create_filesystem_object(struct archive_write_disk *a)
{
	/* Create the entry. */
	const char *linkname;
	mode_t final_mode, mode;
	int r;

	/* We identify hard/symlinks according to the link names. */
	/* Since link(2) and symlink(2) don't handle modes, we're done here. */
	linkname = archive_entry_hardlink(a->entry);
	if (linkname != NULL) {
		r = link(linkname, a->name) ? errno : 0;
		/*
		 * New cpio and pax formats allow hardlink entries
		 * to carry data, so we may have to open the file
		 * for hardlink entries.
		 *
		 * If the hardlink was successfully created and
		 * the archive doesn't have carry data for it,
		 * consider it to be non-authoritive for meta data.
		 * This is consistent with GNU tar and BSD pax.
		 * If the hardlink does carry data, let the last
		 * archive entry decide ownership.
		 */
		if (r == 0 && a->filesize <= 0) {
			a->todo = 0;
			a->deferred = 0;
		} if (r == 0 && a->filesize > 0) {
			a->fd = open(a->name, O_WRONLY | O_TRUNC | O_BINARY);
			if (a->fd < 0)
				r = errno;
		}
		return (r);
	}
	linkname = archive_entry_symlink(a->entry);
	if (linkname != NULL) {
#if HAVE_SYMLINK
		return symlink(linkname, a->name) ? errno : 0;
#else
		return (EPERM);
#endif
	}

	/*
	 * The remaining system calls all set permissions, so let's
	 * try to take advantage of that to avoid an extra chmod()
	 * call.  (Recall that umask is set to zero right now!)
	 */

	/* Mode we want for the final restored object (w/o file type bits). */
	final_mode = a->mode & 07777;
	/*
	 * The mode that will actually be restored in this step.  Note
	 * that SUID, SGID, etc, require additional work to ensure
	 * security, so we never restore them at this point.
	 */
	mode = final_mode & 0777 & a->user_umask;

	switch (a->mode & AE_IFMT) {
	default:
		/* POSIX requires that we fall through here. */
		/* FALLTHROUGH */
	case AE_IFREG:
		a->fd = open(a->name,
		    O_WRONLY | O_CREAT | O_EXCL | O_BINARY, mode);
		r = (a->fd < 0);
		break;
	case AE_IFCHR:
	case AE_IFBLK:
		/* TODO: Find a better way to warn about our inability
		 * to restore a block device node. */
		return (EINVAL);
	case AE_IFDIR:
		mode = (mode | MINIMUM_DIR_MODE) & MAXIMUM_DIR_MODE;
		r = mkdir(a->name, mode);
		if (r == 0) {
			/* Defer setting dir times. */
			a->deferred |= (a->todo & TODO_TIMES);
			a->todo &= ~TODO_TIMES;
			/* Never use an immediate chmod(). */
			/* We can't avoid the chmod() entirely if EXTRACT_PERM
			 * because of SysV SGID inheritance. */
			if ((mode != final_mode)
			    || (a->flags & ARCHIVE_EXTRACT_PERM))
				a->deferred |= (a->todo & TODO_MODE);
			a->todo &= ~TODO_MODE;
		}
		break;
	case AE_IFIFO:
		/* TODO: Find a better way to warn about our inability
		 * to restore a fifo. */
		return (EINVAL);
	}

	/* All the system calls above set errno on failure. */
	if (r)
		return (errno);

	/* If we managed to set the final mode, we've avoided a chmod(). */
	if (mode == final_mode)
		a->todo &= ~TODO_MODE;
	return (0);
}

/*
 * Cleanup function for archive_extract.  Mostly, this involves processing
 * the fixup list, which is used to address a number of problems:
 *   * Dir permissions might prevent us from restoring a file in that
 *     dir, so we restore the dir with minimum 0700 permissions first,
 *     then correct the mode at the end.
 *   * Similarly, the act of restoring a file touches the directory
 *     and changes the timestamp on the dir, so we have to touch-up dir
 *     timestamps at the end as well.
 *   * Some file flags can interfere with the restore by, for example,
 *     preventing the creation of hardlinks to those files.
 *   * Mac OS extended metadata includes ACLs, so must be deferred on dirs.
 *
 * Note that tar/cpio do not require that archives be in a particular
 * order; there is no way to know when the last file has been restored
 * within a directory, so there's no way to optimize the memory usage
 * here by fixing up the directory any earlier than the
 * end-of-archive.
 *
 * XXX TODO: Directory ACLs should be restored here, for the same
 * reason we set directory perms here. XXX
 */
static int
_archive_write_disk_close(struct archive *_a)
{
	struct archive_write_disk *a = (struct archive_write_disk *)_a;
	struct fixup_entry *next, *p;
	int ret;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_write_disk_close");
	ret = _archive_write_disk_finish_entry(&a->archive);

	/* Sort dir list so directories are fixed up in depth-first order. */
	p = sort_dir_list(a->fixup_list);

	while (p != NULL) {
		a->pst = NULL; /* Mark stat cache as out-of-date. */
		if (p->fixup & TODO_TIMES) {
			set_times(a, -1, p->mode, p->name,
			    p->atime, p->atime_nanos,
			    p->birthtime, p->birthtime_nanos,
			    p->mtime, p->mtime_nanos,
			    p->ctime, p->ctime_nanos);
		}
		if (p->fixup & TODO_MODE_BASE)
			chmod(p->name, p->mode);
		if (p->fixup & TODO_ACLS)
			set_acls(a, -1, p->name, &p->acl);
		next = p->next;
		archive_acl_clear(&p->acl);
		free(p->mac_metadata);
		free(p->name);
		free(p);
		p = next;
	}
	a->fixup_list = NULL;
	return (ret);
}

static int
_archive_write_disk_free(struct archive *_a)
{
	struct archive_write_disk *a;
	int ret;
	if (_a == NULL)
		return (ARCHIVE_OK);
	archive_check_magic(_a, ARCHIVE_WRITE_DISK_MAGIC,
	    ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL, "archive_write_disk_free");
	a = (struct archive_write_disk *)_a;
	ret = _archive_write_disk_close(&a->archive);
	if (a->cleanup_gid != NULL && a->lookup_gid_data != NULL)
		(a->cleanup_gid)(a->lookup_gid_data);
	if (a->cleanup_uid != NULL && a->lookup_uid_data != NULL)
		(a->cleanup_uid)(a->lookup_uid_data);
	if (a->entry)
		archive_entry_free(a->entry);
	archive_string_free(&a->_name_data);
	archive_string_free(&a->archive.error_string);
	archive_string_free(&a->path_safe);
	a->archive.magic = 0;
	__archive_clean(&a->archive);
	free(a);
	return (ret);
}

/*
 * Simple O(n log n) merge sort to order the fixup list.  In
 * particular, we want to restore dir timestamps depth-first.
 */
static struct fixup_entry *
sort_dir_list(struct fixup_entry *p)
{
	struct fixup_entry *a, *b, *t;

	if (p == NULL)
		return (NULL);
	/* A one-item list is already sorted. */
	if (p->next == NULL)
		return (p);

	/* Step 1: split the list. */
	t = p;
	a = p->next->next;
	while (a != NULL) {
		/* Step a twice, t once. */
		a = a->next;
		if (a != NULL)
			a = a->next;
		t = t->next;
	}
	/* Now, t is at the mid-point, so break the list here. */
	b = t->next;
	t->next = NULL;
	a = p;

	/* Step 2: Recursively sort the two sub-lists. */
	a = sort_dir_list(a);
	b = sort_dir_list(b);

	/* Step 3: Merge the returned lists. */
	/* Pick the first element for the merged list. */
	if (strcmp(a->name, b->name) > 0) {
		t = p = a;
		a = a->next;
	} else {
		t = p = b;
		b = b->next;
	}

	/* Always put the later element on the list first. */
	while (a != NULL && b != NULL) {
		if (strcmp(a->name, b->name) > 0) {
			t->next = a;
			a = a->next;
		} else {
			t->next = b;
			b = b->next;
		}
		t = t->next;
	}

	/* Only one list is non-empty, so just splice it on. */
	if (a != NULL)
		t->next = a;
	if (b != NULL)
		t->next = b;

	return (p);
}

/*
 * Returns a new, initialized fixup entry.
 *
 * TODO: Reduce the memory requirements for this list by using a tree
 * structure rather than a simple list of names.
 */
static struct fixup_entry *
new_fixup(struct archive_write_disk *a, const char *pathname)
{
	struct fixup_entry *fe;

	fe = (struct fixup_entry *)calloc(1, sizeof(struct fixup_entry));
	if (fe == NULL)
		return (NULL);
	fe->next = a->fixup_list;
	a->fixup_list = fe;
	fe->fixup = 0;
	fe->name = strdup(pathname);
	return (fe);
}

/*
 * Returns a fixup structure for the current entry.
 */
static struct fixup_entry *
current_fixup(struct archive_write_disk *a, const char *pathname)
{
	if (a->current_fixup == NULL)
		a->current_fixup = new_fixup(a, pathname);
	return (a->current_fixup);
}

/* TODO: Make this work. */
/*
 * TODO: The deep-directory support bypasses this; disable deep directory
 * support if we're doing symlink checks.
 */
/*
 * TODO: Someday, integrate this with the deep dir support; they both
 * scan the path and both can be optimized by comparing against other
 * recent paths.
 */
/* TODO: Extend this to support symlinks on Windows Vista and later. */
static int
check_symlinks(struct archive_write_disk *a)
{
#if !defined(HAVE_LSTAT)
	/* Platform doesn't have lstat, so we can't look for symlinks. */
	(void)a; /* UNUSED */
	return (ARCHIVE_OK);
#else
	char *pn, *p;
	char c;
	int r;
	struct stat st;

	/*
	 * Guard against symlink tricks.  Reject any archive entry whose
	 * destination would be altered by a symlink.
	 */
	/* Whatever we checked last time doesn't need to be re-checked. */
	pn = a->name;
	p = a->path_safe.s;
	while ((*pn != '\0') && (*p == *pn))
		++p, ++pn;
	c = pn[0];
	/* Keep going until we've checked the entire name. */
	while (pn[0] != '\0' && (pn[0] != '/' || pn[1] != '\0')) {
		/* Skip the next path element. */
		while (*pn != '\0' && *pn != '/')
			++pn;
		c = pn[0];
		pn[0] = '\0';
		/* Check that we haven't hit a symlink. */
		r = lstat(a->name, &st);
		if (r != 0) {
			/* We've hit a dir that doesn't exist; stop now. */
			if (errno == ENOENT)
				break;
		} else if (S_ISLNK(st.st_mode)) {
			if (c == '\0') {
				/*
				 * Last element is symlink; remove it
				 * so we can overwrite it with the
				 * item being extracted.
				 */
				if (unlink(a->name)) {
					archive_set_error(&a->archive, errno,
					    "Could not remove symlink %s",
					    a->name);
					pn[0] = c;
					return (ARCHIVE_FAILED);
				}
				a->pst = NULL;
				/*
				 * Even if we did remove it, a warning
				 * is in order.  The warning is silly,
				 * though, if we're just replacing one
				 * symlink with another symlink.
				 */
				if (!S_ISLNK(a->mode)) {
					archive_set_error(&a->archive, 0,
					    "Removing symlink %s",
					    a->name);
				}
				/* Symlink gone.  No more problem! */
				pn[0] = c;
				return (0);
			} else if (a->flags & ARCHIVE_EXTRACT_UNLINK) {
				/* User asked us to remove problems. */
				if (unlink(a->name) != 0) {
					archive_set_error(&a->archive, 0,
					    "Cannot remove intervening symlink %s",
					    a->name);
					pn[0] = c;
					return (ARCHIVE_FAILED);
				}
				a->pst = NULL;
			} else {
				archive_set_error(&a->archive, 0,
				    "Cannot extract through symlink %s",
				    a->name);
				pn[0] = c;
				return (ARCHIVE_FAILED);
			}
		}
	}
	pn[0] = c;
	/* We've checked and/or cleaned the whole path, so remember it. */
	archive_strcpy(&a->path_safe, a->name);
	return (ARCHIVE_OK);
#endif
}

/*
 * 1. Convert a path separator from '\' to '/' .
 *    We shouldn't check multi-byte character directly because some
 *    character-set have been using the '\' character for a part of
 *    its multibyte character code.
 * 2. Replace unusable characters in Windows with underscore('_').
 * See also : http://msdn.microsoft.com/en-us/library/aa365247.aspx
 */
static void
cleanup_pathname_win(struct archive_write_disk *a)
{
	wchar_t wc;
	char *p;
	size_t alen, l;
	int mb, dos;

	alen = 0;
	mb = dos = 0;
	for (p = a->name; *p != '\0'; p++) {
		++alen;
		if (*(unsigned char *)p > 127)
			mb = 1;
		if (*p == '\\') {
			/* If we have not met any multi-byte characters,
			 * we can replace '\' with '/'. */
			if (!mb)
				*p = '/';
			dos = 1;
		}
		/* Rewrite the path name if its character is a unusable. */
		if (*p == ':' || *p == '*' || *p == '?' || *p == '"' ||
		    *p == '<' || *p == '>' || *p == '|')
			*p = '_';
	}
	if (!mb || !dos)
		return;

	/*
	 * Convert path separator in wide-character.
	 */
	p = a->name;
	while (*p != '\0' && alen) {
		l = mbtowc(&wc, p, alen);
		if (l == -1) {
			while (*p != '\0') {
				if (*p == '\\')
					*p = '/';
				++p;
			}
			break;
		}
		if (l == 1 && wc == L'\\')
			*p = '/';
		p += l;
		alen -= l;
	}
}

/*
 * Canonicalize the pathname.  In particular, this strips duplicate
 * '/' characters, '.' elements, and trailing '/'.  It also raises an
 * error for an empty path, a trailing '..' or (if _SECURE_NODOTDOT is
 * set) any '..' in the path.
 */
static int
cleanup_pathname(struct archive_write_disk *a)
{
	char *dest, *src;
	char separator = '\0';

	dest = src = a->name;
	if (*src == '\0') {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid empty pathname");
		return (ARCHIVE_FAILED);
	}

	cleanup_pathname_win(a);
	/* Skip leading '/'. */
	if (*src == '/')
		separator = *src++;

	/* Scan the pathname one element at a time. */
	for (;;) {
		/* src points to first char after '/' */
		if (src[0] == '\0') {
			break;
		} else if (src[0] == '/') {
			/* Found '//', ignore second one. */
			src++;
			continue;
		} else if (src[0] == '.') {
			if (src[1] == '\0') {
				/* Ignore trailing '.' */
				break;
			} else if (src[1] == '/') {
				/* Skip './'. */
				src += 2;
				continue;
			} else if (src[1] == '.') {
				if (src[2] == '/' || src[2] == '\0') {
					/* Conditionally warn about '..' */
					if (a->flags & ARCHIVE_EXTRACT_SECURE_NODOTDOT) {
						archive_set_error(&a->archive,
						    ARCHIVE_ERRNO_MISC,
						    "Path contains '..'");
						return (ARCHIVE_FAILED);
					}
				}
				/*
				 * Note: Under no circumstances do we
				 * remove '..' elements.  In
				 * particular, restoring
				 * '/foo/../bar/' should create the
				 * 'foo' dir as a side-effect.
				 */
			}
		}

		/* Copy current element, including leading '/'. */
		if (separator)
			*dest++ = '/';
		while (*src != '\0' && *src != '/') {
			*dest++ = *src++;
		}

		if (*src == '\0')
			break;

		/* Skip '/' separator. */
		separator = *src++;
	}
	/*
	 * We've just copied zero or more path elements, not including the
	 * final '/'.
	 */
	if (dest == a->name) {
		/*
		 * Nothing got copied.  The path must have been something
		 * like '.' or '/' or './' or '/././././/./'.
		 */
		if (separator)
			*dest++ = '/';
		else
			*dest++ = '.';
	}
	/* Terminate the result. */
	*dest = '\0';
	return (ARCHIVE_OK);
}

/*
 * Create the parent directory of the specified path, assuming path
 * is already in mutable storage.
 */
static int
create_parent_dir(struct archive_write_disk *a, char *path)
{
	char *slash;
	int r;

	/* Remove tail element to obtain parent name. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return (ARCHIVE_OK);
	*slash = '\0';
	r = create_dir(a, path);
	*slash = '/';
	return (r);
}

/*
 * Create the specified dir, recursing to create parents as necessary.
 *
 * Returns ARCHIVE_OK if the path exists when we're done here.
 * Otherwise, returns ARCHIVE_FAILED.
 * Assumes path is in mutable storage; path is unchanged on exit.
 */
static int
create_dir(struct archive_write_disk *a, char *path)
{
	struct stat st;
	struct fixup_entry *le;
	char *slash, *base;
	mode_t mode_final, mode;
	int r;

	/* Check for special names and just skip them. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		base = path;
	else
		base = slash + 1;

	if (base[0] == '\0' ||
	    (base[0] == '.' && base[1] == '\0') ||
	    (base[0] == '.' && base[1] == '.' && base[2] == '\0')) {
		/* Don't bother trying to create null path, '.', or '..'. */
		if (slash != NULL) {
			*slash = '\0';
			r = create_dir(a, path);
			*slash = '/';
			return (r);
		}
		return (ARCHIVE_OK);
	}

	/*
	 * Yes, this should be stat() and not lstat().  Using lstat()
	 * here loses the ability to extract through symlinks.  Also note
	 * that this should not use the a->st cache.
	 */
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return (ARCHIVE_OK);
		if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE)) {
			archive_set_error(&a->archive, EEXIST,
			    "Can't create directory '%s'", path);
			return (ARCHIVE_FAILED);
		}
		if (unlink(path) != 0) {
			archive_set_error(&a->archive, errno,
			    "Can't create directory '%s': "
			    "Conflicting file cannot be removed",
			    path);
			return (ARCHIVE_FAILED);
		}
	} else if (errno != ENOENT && errno != ENOTDIR) {
		/* Stat failed? */
		archive_set_error(&a->archive, errno, "Can't test directory '%s'", path);
		return (ARCHIVE_FAILED);
	} else if (slash != NULL) {
		*slash = '\0';
		r = create_dir(a, path);
		*slash = '/';
		if (r != ARCHIVE_OK)
			return (r);
	}

	/*
	 * Mode we want for the final restored directory.  Per POSIX,
	 * implicitly-created dirs must be created obeying the umask.
	 * There's no mention whether this is different for privileged
	 * restores (which the rest of this code handles by pretending
	 * umask=0).  I've chosen here to always obey the user's umask for
	 * implicit dirs, even if _EXTRACT_PERM was specified.
	 */
	mode_final = DEFAULT_DIR_MODE & ~a->user_umask;
	/* Mode we want on disk during the restore process. */
	mode = mode_final;
	mode |= MINIMUM_DIR_MODE;
	mode &= MAXIMUM_DIR_MODE;
	if (mkdir(path, mode) == 0) {
		if (mode != mode_final) {
			le = new_fixup(a, path);
			le->fixup |=TODO_MODE_BASE;
			le->mode = mode_final;
		}
		return (ARCHIVE_OK);
	}

	/*
	 * Without the following check, a/b/../b/c/d fails at the
	 * second visit to 'b', so 'd' can't be created.  Note that we
	 * don't add it to the fixup list here, as it's already been
	 * added.
	 */
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return (ARCHIVE_OK);

	archive_set_error(&a->archive, errno, "Failed to create dir '%s'",
	    path);
	return (ARCHIVE_FAILED);
}

/*
 * Note: Although we can skip setting the user id if the desired user
 * id matches the current user, we cannot skip setting the group, as
 * many systems set the gid based on the containing directory.  So
 * we have to perform a chown syscall if we want to set the SGID
 * bit.  (The alternative is to stat() and then possibly chown(); it's
 * more efficient to skip the stat() and just always chown().)  Note
 * that a successful chown() here clears the TODO_SGID_CHECK bit, which
 * allows set_mode to skip the stat() check for the GID.
 */
static int
set_ownership(struct archive_write_disk *a)
{
/* unfortunately, on win32 there is no 'root' user with uid 0,
   so we just have to try the chown and see if it works */

	/* If we know we can't change it, don't bother trying. */
	if (a->user_uid != 0  &&  a->user_uid != a->uid) {
		archive_set_error(&a->archive, errno,
		    "Can't set UID=%jd", (intmax_t)a->uid);
		return (ARCHIVE_WARN);
	}

	archive_set_error(&a->archive, errno,
	    "Can't set user=%jd/group=%jd for %s",
	    (intmax_t)a->uid, (intmax_t)a->gid, a->name);
	return (ARCHIVE_WARN);
}

static int
set_times(struct archive_write_disk *a,
    int fd, int mode, const char *name,
    time_t atime, long atime_nanos,
    time_t birthtime, long birthtime_nanos,
    time_t mtime, long mtime_nanos,
    time_t ctime, long ctime_nanos)
{
#define EPOC_TIME ARCHIVE_LITERAL_ULL(116444736000000000)
#define WINTIME(sec, nsec) ((Int32x32To64(sec, 10000000) + EPOC_TIME)\
	 + (((nsec)/1000)*10))

	HANDLE h, hw;
	ULARGE_INTEGER wintm;
	wchar_t *ws;
	FILETIME *pfbtime;
	FILETIME fatime, fbtime, fmtime;

	if (fd >= 0) {
		h = (HANDLE)_get_osfhandle(fd);
		hw = NULL;
		ws = NULL;
	} else {
		if (S_ISLNK(mode))
			return (ARCHIVE_OK);
		ws = __la_win_permissive_name(name);
		if (ws == NULL)
			goto settimes_failed;
		hw = CreateFileW(ws, FILE_WRITE_ATTRIBUTES,
		    0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (hw == INVALID_HANDLE_VALUE)
			goto settimes_failed;
		h = hw;
	}

	wintm.QuadPart = WINTIME(atime, atime_nanos);
	fatime.dwLowDateTime = wintm.LowPart;
	fatime.dwHighDateTime = wintm.HighPart;
	wintm.QuadPart = WINTIME(mtime, mtime_nanos);
	fmtime.dwLowDateTime = wintm.LowPart;
	fmtime.dwHighDateTime = wintm.HighPart;
	/*
	 * SetFileTime() supports birthtime.
	 */
	if (birthtime > 0 || birthtime_nanos > 0) {
		wintm.QuadPart = WINTIME(birthtime, birthtime_nanos);
		fbtime.dwLowDateTime = wintm.LowPart;
		fbtime.dwHighDateTime = wintm.HighPart;
		pfbtime = &fbtime;
	} else
		pfbtime = NULL;
	if (SetFileTime(h, pfbtime, &fatime, &fmtime) == 0)
		goto settimes_failed;
	free(ws);
	CloseHandle(hw);
	return (ARCHIVE_OK);

settimes_failed:
	free(ws);
	CloseHandle(hw);
	archive_set_error(&a->archive, EINVAL, "Can't restore time");
	return (ARCHIVE_WARN);
}

static int
set_times_from_entry(struct archive_write_disk *a)
{
	time_t atime, birthtime, mtime, ctime;
	long atime_nsec, birthtime_nsec, mtime_nsec, ctime_nsec;

	/* Suitable defaults. */
	atime = birthtime = mtime = ctime = a->start_time;
	atime_nsec = birthtime_nsec = mtime_nsec = ctime_nsec = 0;

	/* If no time was provided, we're done. */
	if (!archive_entry_atime_is_set(a->entry)
	    && !archive_entry_birthtime_is_set(a->entry)
	    && !archive_entry_mtime_is_set(a->entry))
		return (ARCHIVE_OK);

	if (archive_entry_atime_is_set(a->entry)) {
		atime = archive_entry_atime(a->entry);
		atime_nsec = archive_entry_atime_nsec(a->entry);
	}
	if (archive_entry_birthtime_is_set(a->entry)) {
		birthtime = archive_entry_birthtime(a->entry);
		birthtime_nsec = archive_entry_birthtime_nsec(a->entry);
	}
	if (archive_entry_mtime_is_set(a->entry)) {
		mtime = archive_entry_mtime(a->entry);
		mtime_nsec = archive_entry_mtime_nsec(a->entry);
	}
	if (archive_entry_ctime_is_set(a->entry)) {
		ctime = archive_entry_ctime(a->entry);
		ctime_nsec = archive_entry_ctime_nsec(a->entry);
	}

	return set_times(a, a->fd, a->mode, a->name,
			 atime, atime_nsec,
			 birthtime, birthtime_nsec,
			 mtime, mtime_nsec,
			 ctime, ctime_nsec);
}

static int
set_mode(struct archive_write_disk *a, int mode)
{
	int r = ARCHIVE_OK;
	mode &= 07777; /* Strip off file type bits. */

	if (a->todo & TODO_SGID_CHECK) {
		/*
		 * If we don't know the GID is right, we must stat()
		 * to verify it.  We can't just check the GID of this
		 * process, since systems sometimes set GID from
		 * the enclosing dir or based on ACLs.
		 */
		if ((r = lazy_stat(a)) != ARCHIVE_OK)
			return (r);
		if (a->pst->st_gid != a->gid) {
			mode &= ~ S_ISGID;
		}
		/* While we're here, double-check the UID. */
		if (a->pst->st_uid != a->uid
		    && (a->todo & TODO_SUID)) {
			mode &= ~ S_ISUID;
		}
		a->todo &= ~TODO_SGID_CHECK;
		a->todo &= ~TODO_SUID_CHECK;
	} else if (a->todo & TODO_SUID_CHECK) {
		/*
		 * If we don't know the UID is right, we can just check
		 * the user, since all systems set the file UID from
		 * the process UID.
		 */
		if (a->user_uid != a->uid) {
			mode &= ~ S_ISUID;
		}
		a->todo &= ~TODO_SUID_CHECK;
	}

	if (S_ISLNK(a->mode)) {
#ifdef HAVE_LCHMOD
		/*
		 * If this is a symlink, use lchmod().  If the
		 * platform doesn't support lchmod(), just skip it.  A
		 * platform that doesn't provide a way to set
		 * permissions on symlinks probably ignores
		 * permissions on symlinks, so a failure here has no
		 * impact.
		 */
		if (lchmod(a->name, mode) != 0) {
			archive_set_error(&a->archive, errno,
			    "Can't set permissions to 0%o", (int)mode);
			r = ARCHIVE_WARN;
		}
#endif
	} else if (!S_ISDIR(a->mode)) {
		/*
		 * If it's not a symlink and not a dir, then use
		 * fchmod() or chmod(), depending on whether we have
		 * an fd.  Dirs get their perms set during the
		 * post-extract fixup, which is handled elsewhere.
		 */
#ifdef HAVE_FCHMOD
		if (a->fd >= 0) {
			if (fchmod(a->fd, mode) != 0) {
				archive_set_error(&a->archive, errno,
				    "Can't set permissions to 0%o", (int)mode);
				r = ARCHIVE_WARN;
			}
		} else
#endif
			/* If this platform lacks fchmod(), then
			 * we'll just use chmod(). */
			if (chmod(a->name, mode) != 0) {
				archive_set_error(&a->archive, errno,
				    "Can't set permissions to 0%o", (int)mode);
				r = ARCHIVE_WARN;
			}
	}
	return (r);
}

static int
set_fflags(struct archive_write_disk *a)
{
	(void)a; /* UNUSED */
	return (ARCHIVE_OK);
}

/* Default empty function body to satisfy mainline code. */
static int
set_acls(struct archive_write_disk *a, int fd, const char *name,
	 struct archive_acl *acl)
{
	(void)a; /* UNUSED */
	(void)fd; /* UNUSED */
	(void)name; /* UNUSED */
	(void)acl; /* UNUSED */
	return (ARCHIVE_OK);
}

/*
 * Restore extended attributes - stub implementation for unsupported systems
 */
static int
set_xattrs(struct archive_write_disk *a)
{
	static int warning_done = 0;

	/* If there aren't any extended attributes, then it's okay not
	 * to extract them, otherwise, issue a single warning. */
	if (archive_entry_xattr_count(a->entry) != 0 && !warning_done) {
		warning_done = 1;
		archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
		    "Cannot restore extended attributes on this system");
		return (ARCHIVE_WARN);
	}
	/* Warning was already emitted; suppress further warnings. */
	return (ARCHIVE_OK);
}


/*
 * Trivial implementations of gid/uid lookup functions.
 * These are normally overridden by the client, but these stub
 * versions ensure that we always have something that works.
 */
static int64_t
trivial_lookup_gid(void *private_data, const char *gname, int64_t gid)
{
	(void)private_data; /* UNUSED */
	(void)gname; /* UNUSED */
	return (gid);
}

static int64_t
trivial_lookup_uid(void *private_data, const char *uname, int64_t uid)
{
	(void)private_data; /* UNUSED */
	(void)uname; /* UNUSED */
	return (uid);
}

/*
 * Test if file on disk is older than entry.
 */
static int
older(struct stat *st, struct archive_entry *entry)
{
	/* First, test the seconds and return if we have a definite answer. */
	/* Definitely older. */
	if (st->st_mtime < archive_entry_mtime(entry))
		return (1);
	/* Definitely younger. */
	if (st->st_mtime > archive_entry_mtime(entry))
		return (0);
	/* This system doesn't have high-res timestamps. */
	/* Same age or newer, so not older. */
	return (0);
}

#endif /* _WIN32 && !__CYGWIN__ */
