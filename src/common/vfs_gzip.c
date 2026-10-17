// vfs_gzip.c -- a gzip unpacked as it is read (vfs.h's VFS_OpenGunzip): its
// file read as far as it has come at each call, and what that unpacks to kept,
// to read and seek in, so a download plays as it comes and a file on disk is
// all unpacked at the first call. A file that isn't a gzip (no 1f 8b first) is
// passed through as it is.

#include "vfs.h"
#include "mem.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define GZ_FIRSTROOM	(256 * 1024)

typedef enum { GZ_SNIFFING, GZ_PASSING, GZ_INFLATING, GZ_DONE, GZ_FAILED } gzmode_t;

typedef struct
{
	vfsfile_t	vfs;
	vfsfile_t	*inner;
	long long	base;			// where the inner was at the start
	gzmode_t	mode;
	byte		magic[2];		// its first bytes
	int			sniffed;		// of them
	z_stream	z;
	bool		started;		// inflateInit2'd
	byte		in[16384];		// read from the inner, not yet unpacked
	byte		*out;			// what it has unpacked to
	size_t		size, room, pos;
	char		error[128];
} vfsgzip_t;

static void Gz_Fail (vfsgzip_t *g, const char *why)
{
	g->mode = GZ_FAILED;
	snprintf (g->error, sizeof(g->error), "%s", why);
}

// what is in z's input, unpacked onto the end of out
static void Gz_Inflate (vfsgzip_t *g)
{
	size_t	before;
	uInt	given;
	int		ret;

	while (g->mode == GZ_INFLATING && g->z.avail_in)
	{
		if (g->size == g->room)
		{
			g->room = g->room ? g->room * 2 : GZ_FIRSTROOM;
			g->out = Mem_Realloc (g->out, g->room);
		}
		g->z.next_out = g->out + g->size;
		g->z.avail_out = (uInt)(g->room - g->size > 1u << 30 ? 1u << 30 : g->room - g->size);
		before = g->z.avail_out;
		given = g->z.avail_in;
		ret = inflate (&g->z, Z_NO_FLUSH);
		g->size += before - g->z.avail_out;
		if (ret == Z_STREAM_END)
			g->mode = GZ_DONE;		// what follows the gzip is left alone
		else if (ret != Z_OK && ret != Z_BUF_ERROR)
			Gz_Fail (g, g->z.msg ? g->z.msg : zError (ret));
		else if (g->z.avail_in == given && g->z.avail_out == before)
			Gz_Fail (g, "the gzip goes no further");	// zlib took nothing and gave nothing
	}
}

// what the inner has brought since, unpacked: all of a file that has ended
static void Gz_Pump (vfsgzip_t *g)
{
	int		n;

	if (g->mode == GZ_SNIFFING)
	{
		while (g->sniffed < 2 && (n = VFS_Read (g->inner, g->magic + g->sniffed, 2 - g->sniffed)) > 0)
			g->sniffed += n;
		if (g->sniffed < 2 && VFS_Status (g->inner) != VFS_ENDED)
		{	// more to come, or the inner's failure to show
			if (VFS_Status (g->inner) == VFS_FAILED)
				Gz_Fail (g, VFS_Error (g->inner));
			return;
		}
		if (g->sniffed < 2 || g->magic[0] != 0x1f || g->magic[1] != 0x8b)
		{	// not a gzip: read again from its start, as it is
			g->mode = GZ_PASSING;
			if (!VFS_Seek (g->inner, g->base))
				Gz_Fail (g, "it can't be read again from its start");
			return;
		}
		// windowBits 15 + 16: a gzip's header and trailer
		if (inflateInit2 (&g->z, 15 + 16) != Z_OK)
		{
			Gz_Fail (g, "zlib can't start");
			return;
		}
		g->started = true;
		g->mode = GZ_INFLATING;
		g->z.next_in = g->magic;
		g->z.avail_in = 2;
		Gz_Inflate (g);
	}

	while (g->mode == GZ_INFLATING)
	{
		n = VFS_Read (g->inner, g->in, sizeof(g->in));
		if (n <= 0)
		{
			if (VFS_Status (g->inner) == VFS_FAILED)
				Gz_Fail (g, VFS_Error (g->inner));
			else if (VFS_Status (g->inner) == VFS_ENDED)
				Gz_Fail (g, "the gzip is cut short");
			return;
		}
		g->z.next_in = g->in;
		g->z.avail_in = (uInt)n;
		Gz_Inflate (g);
	}
}

static int Gz_Read (vfsfile_t *f, void *buf, int size)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;
	size_t		n;

	if (g->pos == g->size)
		Gz_Pump (g);
	if (g->mode == GZ_PASSING)
		return VFS_Read (g->inner, buf, size);
	n = g->size - g->pos;
	if (n > (size_t)size)
		n = (size_t)size;
	memcpy (buf, g->out + g->pos, n);
	g->pos += n;
	return (int)n;
}

static bool Gz_Seek (vfsfile_t *f, long long pos)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;

	Gz_Pump (g);
	if (g->mode == GZ_PASSING)
		return VFS_Seek (g->inner, g->base + pos);
	if ((unsigned long long)pos > g->size)
		return false;
	g->pos = (size_t)pos;
	return true;
}

static long long Gz_Tell (vfsfile_t *f)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;

	if (g->mode == GZ_PASSING)
		return VFS_Tell (g->inner) - g->base;
	return (long long)g->pos;
}

static long long Gz_Length (vfsfile_t *f)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;
	long long	length;

	Gz_Pump (g);
	if (g->mode == GZ_PASSING)
		return (length = VFS_Length (g->inner)) >= 0 ? length - g->base : -1;
	return g->mode == GZ_DONE ? (long long)g->size : -1;
}

static vfsstatus_t Gz_Status (vfsfile_t *f)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;

	Gz_Pump (g);
	switch (g->mode)
	{
	case GZ_PASSING:	return VFS_Status (g->inner);
	case GZ_DONE:		return VFS_ENDED;
	case GZ_FAILED:		return VFS_FAILED;
	default:			return VFS_LIVE;
	}
}

static const char *Gz_Error (vfsfile_t *f)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;

	if (g->mode == GZ_PASSING)
		return VFS_Error (g->inner);
	return g->mode == GZ_FAILED ? g->error : "";
}

static void Gz_Close (vfsfile_t *f)
{
	vfsgzip_t	*g = (vfsgzip_t *)f;

	if (g->started)
		inflateEnd (&g->z);
	Mem_Free (g->out);
	VFS_Close (g->inner);
	Mem_Free (g);
}

vfsfile_t *VFS_OpenGunzip (vfsfile_t *inner)
{
	vfsgzip_t	*g = Mem_Calloc (1, sizeof(*g));

	g->vfs = (vfsfile_t){Gz_Read, Gz_Seek, Gz_Tell, Gz_Length, Gz_Status, Gz_Error, Gz_Close};
	g->inner = inner;
	g->base = VFS_Tell (inner);
	g->mode = GZ_SNIFFING;
	return &g->vfs;
}
