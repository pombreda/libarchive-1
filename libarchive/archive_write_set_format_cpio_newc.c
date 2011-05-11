/*-
 * Copyright (c) 2003-2007 Tim Kientzle
 * Copyright (c) 2006 Rudolf Marek SYSGO s.r.o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
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
__FBSDID("$FreeBSD: head/lib/libarchive/archive_write_set_format_cpio_newc.c 201160 2009-12-29 05:41:57Z kientzle $");

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "archive.h"
#include "archive_entry.h"
#include "archive_entry_locale.h"
#include "archive_private.h"
#include "archive_write_private.h"

static ssize_t	archive_write_newc_data(struct archive_write *,
		    const void *buff, size_t s);
static int	archive_write_newc_close(struct archive_write *);
static int	archive_write_newc_free(struct archive_write *);
static int	archive_write_newc_finish_entry(struct archive_write *);
static int	archive_write_newc_header(struct archive_write *,
		    struct archive_entry *);
static int      archive_write_newc_options(struct archive_write *,
		    const char *, const char *);
static int	format_hex(int64_t, void *, int);
static int64_t	format_hex_recursive(int64_t, char *, int);
static int	write_header(struct archive_write *, struct archive_entry *);

struct cpio {
	uint64_t	  entry_bytes_remaining;
	int		  padding;

	struct archive_string_conv *opt_sconv;
	struct archive_string_conv *sconv_default;
	int		  init_default_conversion;
};

struct cpio_header_newc {
	char	c_magic[6];
	char	c_ino[8];
	char	c_mode[8];
	char	c_uid[8];
	char	c_gid[8];
	char	c_nlink[8];
	char	c_mtime[8];
	char	c_filesize[8];
	char	c_devmajor[8];
	char	c_devminor[8];
	char	c_rdevmajor[8];
	char	c_rdevminor[8];
	char	c_namesize[8];
	char	c_checksum[8];
};

/* Logic trick: difference between 'n' and next multiple of 4 */
#define PAD4(n)	(3 & (1 + ~(n)))

/*
 * Set output format to 'cpio' format.
 */
int
archive_write_set_format_cpio_newc(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct cpio *cpio;

	archive_check_magic(_a, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_format_cpio_newc");

	/* If someone else was already registered, unregister them. */
	if (a->format_free != NULL)
		(a->format_free)(a);

	cpio = (struct cpio *)malloc(sizeof(*cpio));
	if (cpio == NULL) {
		archive_set_error(&a->archive, ENOMEM, "Can't allocate cpio data");
		return (ARCHIVE_FATAL);
	}
	memset(cpio, 0, sizeof(*cpio));
	a->format_data = cpio;
	a->format_name = "cpio";
	a->format_options = archive_write_newc_options;
	a->format_write_header = archive_write_newc_header;
	a->format_write_data = archive_write_newc_data;
	a->format_finish_entry = archive_write_newc_finish_entry;
	a->format_close = archive_write_newc_close;
	a->format_free = archive_write_newc_free;
	a->archive.archive_format = ARCHIVE_FORMAT_CPIO_SVR4_NOCRC;
	a->archive.archive_format_name = "SVR4 cpio nocrc";
	return (ARCHIVE_OK);
}

static int
archive_write_newc_options(struct archive_write *a, const char *key,
    const char *val)
{
	struct cpio *cpio = (struct cpio *)a->format_data;
	int ret = ARCHIVE_FAILED;

	if (strcmp(key, "hdrcharset")  == 0) {
		if (val == NULL || val[0] == 0)
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "%s: hdrcharset option needs a character-set name",
			    a->format_name);
		else {
			cpio->opt_sconv = archive_string_conversion_to_charset(
			    &a->archive, val, 0);
			if (cpio->opt_sconv != NULL)
				ret = ARCHIVE_OK;
			else
				ret = ARCHIVE_FATAL;
		}
	} else
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "%s: unknown keyword ``%s''", a->format_name, key);

	return (ret);
}

static struct archive_string_conv *
get_sconv(struct archive_write *a)
{
	struct cpio *cpio;
	struct archive_string_conv *sconv;

	cpio = (struct cpio *)a->format_data;
	sconv = cpio->opt_sconv;
	if (sconv == NULL) {
		if (!cpio->init_default_conversion) {
			cpio->sconv_default =
			    archive_string_default_conversion_for_write(
			      &(a->archive));
			cpio->init_default_conversion = 1;
		}
		sconv = cpio->sconv_default;
	}
	return (sconv);
}

static int
archive_write_newc_header(struct archive_write *a, struct archive_entry *entry)
{
	struct cpio *cpio;
	const char *path;
	size_t len;

	if (archive_entry_filetype(entry) == 0) {
		archive_set_error(&a->archive, -1, "Filetype required");
		return (ARCHIVE_FAILED);
	}

	cpio = (struct cpio *)a->format_data;
	(void)archive_entry_pathname_l(entry, &path, &len, get_sconv(a));
	if (len == 0 || path == NULL || path[0] == '\0') {
		archive_set_error(&a->archive, -1, "Pathname required");
		return (ARCHIVE_FAILED);
	}

	if (archive_entry_hardlink(entry) == NULL
	    && (!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0)) {
		archive_set_error(&a->archive, -1, "Size required");
		return (ARCHIVE_FAILED);
	}
	return write_header(a, entry);
}

static int
write_header(struct archive_write *a, struct archive_entry *entry)
{
	int64_t ino;
	struct cpio *cpio;
	const char *p, *path;
	int pathlength, ret, ret_final;
	struct cpio_header_newc	 h;
	struct archive_string_conv *sconv;
	size_t len;
	int pad;

	cpio = (struct cpio *)a->format_data;
	ret_final = ARCHIVE_OK;
	sconv = get_sconv(a);

	ret = archive_entry_pathname_l(entry, &path, &len, sconv);
	if (ret != 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
		    "Can't translate pathname '%s' to %s",
		    archive_entry_pathname(entry),
		    archive_string_conversion_charset_name(sconv));
		ret_final = ARCHIVE_WARN;
	}
	pathlength = (int)len + 1; /* Include trailing null. */

	memset(&h, 0, sizeof(h));
	format_hex(0x070701, &h.c_magic, sizeof(h.c_magic));
	format_hex(archive_entry_devmajor(entry), &h.c_devmajor,
	    sizeof(h.c_devmajor));
	format_hex(archive_entry_devminor(entry), &h.c_devminor,
	    sizeof(h.c_devminor));

	ino = archive_entry_ino64(entry);
	if (ino > 0xffffffff) {
		archive_set_error(&a->archive, ERANGE,
		    "large inode number truncated");
		ret_final = ARCHIVE_WARN;
	}

	/* TODO: Set ret_final to ARCHIVE_WARN if any of these overflow. */
	format_hex(ino & 0xffffffff, &h.c_ino, sizeof(h.c_ino));
	format_hex(archive_entry_mode(entry), &h.c_mode, sizeof(h.c_mode));
	format_hex(archive_entry_uid(entry), &h.c_uid, sizeof(h.c_uid));
	format_hex(archive_entry_gid(entry), &h.c_gid, sizeof(h.c_gid));
	format_hex(archive_entry_nlink(entry), &h.c_nlink, sizeof(h.c_nlink));
	if (archive_entry_filetype(entry) == AE_IFBLK
	    || archive_entry_filetype(entry) == AE_IFCHR) {
	    format_hex(archive_entry_rdevmajor(entry), &h.c_rdevmajor, sizeof(h.c_rdevmajor));
	    format_hex(archive_entry_rdevminor(entry), &h.c_rdevminor, sizeof(h.c_rdevminor));
	} else {
	    format_hex(0, &h.c_rdevmajor, sizeof(h.c_rdevmajor));
	    format_hex(0, &h.c_rdevminor, sizeof(h.c_rdevminor));
	}
	format_hex(archive_entry_mtime(entry), &h.c_mtime, sizeof(h.c_mtime));
	format_hex(pathlength, &h.c_namesize, sizeof(h.c_namesize));
	format_hex(0, &h.c_checksum, sizeof(h.c_checksum));

	/* Non-regular files don't store bodies. */
	if (archive_entry_filetype(entry) != AE_IFREG)
		archive_entry_set_size(entry, 0);

	/* Symlinks get the link written as the body of the entry. */
	ret = archive_entry_symlink_l(entry, &p, &len, sconv);
	if (ret != 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
		    "Can't translate linkname '%s' to %s",
		    archive_entry_symlink(entry),
		    archive_string_conversion_charset_name(sconv));
		ret_final = ARCHIVE_WARN;
	}
	if (len > 0 && p != NULL  &&  *p != '\0')
		ret = format_hex(strlen(p), &h.c_filesize,
		    sizeof(h.c_filesize));
	else
		ret = format_hex(archive_entry_size(entry),
		    &h.c_filesize, sizeof(h.c_filesize));
	if (ret) {
		archive_set_error(&a->archive, ERANGE,
		    "File is too large for this format.");
		return (ARCHIVE_FAILED);
	}

	ret = __archive_write_output(a, &h, sizeof(h));
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Pad pathname to even length. */
	ret = __archive_write_output(a, path, pathlength);
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
	pad = PAD4(pathlength + sizeof(struct cpio_header_newc));
	if (pad) {
		ret = __archive_write_output(a, "\0\0\0", pad);
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	cpio->entry_bytes_remaining = archive_entry_size(entry);
	cpio->padding = PAD4(cpio->entry_bytes_remaining);

	/* Write the symlink now. */
	if (p != NULL  &&  *p != '\0') {
		ret = __archive_write_output(a, p, strlen(p));
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
		pad = PAD4(strlen(p));
		ret = __archive_write_output(a, "\0\0\0", pad);
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}
	return (ret_final);
}

static ssize_t
archive_write_newc_data(struct archive_write *a, const void *buff, size_t s)
{
	struct cpio *cpio;
	int ret;

	cpio = (struct cpio *)a->format_data;
	if (s > cpio->entry_bytes_remaining)
		s = cpio->entry_bytes_remaining;

	ret = __archive_write_output(a, buff, s);
	cpio->entry_bytes_remaining -= s;
	if (ret >= 0)
		return (s);
	else
		return (ret);
}

/*
 * Format a number into the specified field.
 */
static int
format_hex(int64_t v, void *p, int digits)
{
	int64_t	max;
	int	ret;

	max = (((int64_t)1) << (digits * 4)) - 1;
	if (v >= 0  &&  v <= max) {
	    format_hex_recursive(v, (char *)p, digits);
	    ret = 0;
	} else {
	    format_hex_recursive(max, (char *)p, digits);
	    ret = -1;
	}
	return (ret);
}

static int64_t
format_hex_recursive(int64_t v, char *p, int s)
{
	if (s == 0)
		return (v);
	v = format_hex_recursive(v, p+1, s-1);
	*p = "0123456789abcdef"[v & 0xf];
	return (v >> 4);
}

static int
archive_write_newc_close(struct archive_write *a)
{
	int er;
	struct archive_entry *trailer;

	trailer = archive_entry_new();
	archive_entry_set_nlink(trailer, 1);
	archive_entry_set_size(trailer, 0);
	archive_entry_set_pathname(trailer, "TRAILER!!!");
	/* Bypass the required data checks. */
	er = write_header(a, trailer);
	archive_entry_free(trailer);
	return (er);
}

static int
archive_write_newc_free(struct archive_write *a)
{
	struct cpio *cpio;

	cpio = (struct cpio *)a->format_data;
	free(cpio);
	a->format_data = NULL;
	return (ARCHIVE_OK);
}

static int
archive_write_newc_finish_entry(struct archive_write *a)
{
	struct cpio *cpio;

	cpio = (struct cpio *)a->format_data;
	return (__archive_write_nulls(a, cpio->entry_bytes_remaining + cpio->padding));
}
