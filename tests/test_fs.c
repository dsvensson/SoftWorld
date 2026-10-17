/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// test_fs.c -- files of a game directory stored gzipped (fs.h): path.gz found
// as path by FS_LoadFile, FS_OpenVFS and FS_FileInfo, unpacked, with the size
// it unpacks to; a plain file over its gzip; listed by the name it is read by;
// COM_FOpenFile not finding it; and one cut short not loading

#include "fs.h"
#include "mem.h"
#include "sys.h"
#include "vfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#define	mkdir(path, mode)	_mkdir (path)
#else
#include <sys/stat.h>
#endif

#define DATASIZE	(200 * 1024 + 77)

static int	failures;

#define CHECK(x)	do { if (!(x)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

void Sys_Printf (char *fmt, ...)
{
	(void)fmt;		// the search's FindFile lines
}

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

static byte	*data;		// DATASIZE bytes, runs and noise in turn

static void MakeData (void)
{
	unsigned	x = 777;
	size_t		i;

	data = malloc (DATASIZE);
	for (i = 0 ; i < DATASIZE ; i++)
	{
		x = x * 1103515245 + 12345;
		data[i] = (i / 4096) & 1 ? (byte)(x >> 16) : (byte)(i / 512);
	}
}

static void WriteFile (const char *path, const void *bytes, size_t size)
{
	FILE	*f = fopen (path, "wb");

	if (!f || fwrite (bytes, 1, size, f) != size)
	{
		printf ("can't write %s\n", path);
		exit (1);
	}
	fclose (f);
}

// bytes as a gzip, written to path; keep of it (all of it: 0)
static void WriteGzip (const char *path, const void *bytes, size_t size, size_t keep)
{
	z_stream	z = {0};
	uLong		room = compressBound ((uLong)size) + 64;
	byte		*out = malloc (room);

	if (deflateInit2 (&z, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		exit (1);
	z.next_in = (Bytef *)bytes;
	z.avail_in = (uInt)size;
	z.next_out = out;
	z.avail_out = (uInt)room;
	if (deflate (&z, Z_FINISH) != Z_STREAM_END)
		exit (1);
	WriteFile (path, out, keep ? keep : z.total_out);
	deflateEnd (&z);
	free (out);
}

typedef struct
{
	char	paths[16][64];
	int		count;
} listing_t;

static void AddPath (void *ctx, const char *path)
{
	listing_t	*l = ctx;

	if (l->count < 16)
		snprintf (l->paths[l->count++], sizeof(l->paths[0]), "%s", path);
}

// how often the listing has path
static int Listed (const listing_t *l, const char *path)
{
	int		i, n = 0;

	for (i = 0 ; i < l->count ; i++)
		n += !strcmp (l->paths[i], path);
	return n;
}

static void TestLoad (void)
{
	byte		*buf;
	int			len = -1;

	// a.dat is only stored gzipped: all of it unpacked
	buf = FS_LoadFile ("a.dat", &len);
	CHECK (buf && len == DATASIZE && !memcmp (buf, data, DATASIZE));
	CHECK (buf && buf[len] == 0);
	Mem_Free (buf);

	// b.txt is there as it is and gzipped: the plain one
	buf = FS_LoadFile ("b.txt", &len);
	CHECK (buf && len == 7 && !memcmp (buf, "plain b", 7));
	Mem_Free (buf);

	// cut.dat's gzip stops short: not loaded
	CHECK (!FS_LoadFile ("cut.dat", &len));

	// a gzip's own name is the gzip as it is
	buf = FS_LoadFile ("a.dat.gz", &len);
	CHECK (buf && len > 2 && buf[0] == 0x1f && buf[1] == 0x8b && len < DATASIZE);
	Mem_Free (buf);

	CHECK (!FS_LoadFile ("none.dat", &len));
}

static void TestOpen (void)
{
	vfsfile_t	*f;
	FILE		*h = NULL;
	byte		*buf;
	size_t		size = 0;

	f = FS_OpenVFS ("a.dat");
	CHECK (f != NULL);
	if (f)
	{
		CHECK (VFS_Status (f) == VFS_ENDED && VFS_Length (f) == DATASIZE);
		buf = VFS_ReadAll (f, &size);
		CHECK (buf && size == DATASIZE && !memcmp (buf, data, DATASIZE));
		Mem_Free (buf);
		VFS_Close (f);
	}

	// COM_FOpenFile's FILE is the file on disk: not the gzip's
	CHECK (COM_FOpenFile ("a.dat", &h) == -1 && !h);
	CHECK (COM_FOpenFile ("b.txt", &h) == 7 && h);
	if (h)
		fclose (h);
}

static void TestInfo (void)
{
	long long	size = 0, mtime = 0;

	CHECK (FS_FileInfo ("a.dat", &size, &mtime) && size == DATASIZE && mtime > 0);
	CHECK (FS_FileInfo ("b.txt", &size, &mtime) && size == 7);
	CHECK (!FS_FileInfo ("none.dat", &size, &mtime));
}

static void TestList (void)
{
	listing_t	l = {0};

	// a gzip by the name it is read by; b.txt as it is and gzipped, twice
	FS_ListPaths ("", (const char *const[]){".dat", ".txt", NULL}, AddPath, &l);
	CHECK (Listed (&l, "a.dat") == 1);
	CHECK (Listed (&l, "b.txt") == 2);
	CHECK (Listed (&l, "cut.dat") == 1);
	CHECK (!Listed (&l, "a.dat.gz"));

	// asked for by their own extension, gzips by their own names
	memset (&l, 0, sizeof(l));
	FS_ListPaths ("", (const char *const[]){".dat.gz", NULL}, AddPath, &l);
	CHECK (Listed (&l, "a.dat.gz") == 1 && Listed (&l, "cut.dat.gz") == 1);
	CHECK (!Listed (&l, "a.dat"));
}

int main (void)
{
	// loose files outside pak files are only found at the top of a shareware
	// game directory
	MakeData ();
	mkdir ("test_fs_data", 0777);
	mkdir ("test_fs_data/id1", 0777);
	WriteGzip ("test_fs_data/id1/a.dat.gz", data, DATASIZE, 0);
	WriteFile ("test_fs_data/id1/b.txt", "plain b", 7);
	WriteGzip ("test_fs_data/id1/b.txt.gz", "gzipped b", 9, 0);
	WriteGzip ("test_fs_data/id1/cut.dat.gz", data, DATASIZE, 1000);

	COM_Init ("test_fs_data");
	TestLoad ();
	TestOpen ();
	TestInfo ();
	TestList ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("all passed\n");
	return 0;
}
