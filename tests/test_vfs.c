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
// test_vfs.c -- the VFS's files (vfs.h): memory, a file's slice on disk, and a
// gzip unpacked over each, over a file that comes in pieces as a download
// does, over one cut short and one that fails; and what isn't a gzip passed
// through as it is

#include "mem.h"
#include "sys.h"
#include "vfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define DATASIZE	(1024 * 1024 + 4321)

static int	failures;

#define CHECK(x)	do { if (!(x)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

static byte		*data;		// DATASIZE bytes, runs and noise in turn
static byte		*gz;		// them as a gzip
static size_t	gzsize;

static void MakeData (void)
{
	unsigned	x = 12345;
	size_t		i;
	z_stream	z = {0};

	data = Mem_Alloc (DATASIZE);
	for (i = 0 ; i < DATASIZE ; i++)
	{
		x = x * 1103515245 + 12345;
		data[i] = (i / 4096) & 1 ? (byte)(i / 512) : (byte)(x >> 24);
	}
	// windowBits 15 + 16: a gzip
	gz = Mem_Alloc (DATASIZE * 2);
	CHECK (deflateInit2 (&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
	z.next_in = data;
	z.avail_in = DATASIZE;
	z.next_out = gz;
	z.avail_out = DATASIZE * 2;
	CHECK (deflate (&z, Z_FINISH) == Z_STREAM_END);
	gzsize = z.total_out;
	deflateEnd (&z);
	CHECK (gz[0] == 0x1f && gz[1] == 0x8b);
}

// all of f from where it is, read in pieces of up to step bytes
static size_t ReadOut (vfsfile_t *f, byte *out, size_t room, int step)
{
	size_t	have = 0;
	int		n;

	while (have < room && (n = VFS_Read (f, out + have, room - have < (size_t)step ? (int)(room - have) : step)) > 0)
		have += (size_t)n;
	return have;
}

/*
==============================================================================

A FILE THAT COMES IN PIECES

==============================================================================
*/

// a download's file: fed bytes of it have come, read 777 at most at a time
typedef struct
{
	vfsfile_t	vfs;
	const byte	*bytes;
	size_t		size, fed, pos;
	bool		fails;		// once what was fed is read
} feeder_t;

static int Feeder_Read (vfsfile_t *f, void *buf, int size)
{
	feeder_t	*d = (feeder_t *)f;
	size_t		n = d->fed - d->pos;

	if (n > 777)
		n = 777;
	if (n > (size_t)size)
		n = (size_t)size;
	memcpy (buf, d->bytes + d->pos, n);
	d->pos += n;
	return (int)n;
}

static bool Feeder_Seek (vfsfile_t *f, long long pos)
{
	feeder_t	*d = (feeder_t *)f;

	if ((size_t)pos > d->fed)
		return false;
	d->pos = (size_t)pos;
	return true;
}

static long long Feeder_Tell (vfsfile_t *f)
{
	return (long long)((feeder_t *)f)->pos;
}

static long long Feeder_Length (vfsfile_t *f)
{
	(void)f;
	return -1;
}

static vfsstatus_t Feeder_Status (vfsfile_t *f)
{
	feeder_t	*d = (feeder_t *)f;

	if (d->fails && d->pos == d->fed)
		return VFS_FAILED;
	return d->fed < d->size ? VFS_LIVE : VFS_ENDED;
}

static const char *Feeder_Error (vfsfile_t *f)
{
	return Feeder_Status (f) == VFS_FAILED ? "the feeder failed" : "";
}

static void Feeder_Close (vfsfile_t *f)
{
	Mem_Free (f);
}

static feeder_t *Feeder (const byte *bytes, size_t size)
{
	feeder_t	*d = Mem_Calloc (1, sizeof(*d));

	d->vfs = (vfsfile_t){Feeder_Read, Feeder_Seek, Feeder_Tell, Feeder_Length, Feeder_Status, Feeder_Error,
		Feeder_Close};
	d->bytes = bytes;
	d->size = size;
	return d;
}

/*
==============================================================================

THE TESTS

==============================================================================
*/

static void TestMemory (void)
{
	byte		text[] = "hello, world", buf[32];
	vfsfile_t	*f = VFS_OpenMemory (text, 12, false);
	byte		*all;
	size_t		size;

	CHECK (VFS_Status (f) == VFS_ENDED);
	CHECK (VFS_Length (f) == 12);
	CHECK (VFS_Read (f, buf, 5) == 5 && !memcmp (buf, "hello", 5));
	CHECK (VFS_Tell (f) == 5);
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 7 && !memcmp (buf, ", world", 7));
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 0);
	CHECK (!VFS_Seek (f, 13));
	CHECK (VFS_Seek (f, 7) && VFS_Tell (f) == 7);
	all = VFS_ReadAll (f, &size);
	CHECK (all && size == 5 && !memcmp (all, "world", 6));	// with the 0 after it
	Mem_Free (all);
	CHECK (*VFS_Error (f) == 0);
	VFS_Close (f);
	VFS_Close (NULL);
}

static void TestStdio (void)
{
	static const char	name[] = "test_vfs.tmp";
	FILE		*file = fopen (name, "wb");
	vfsfile_t	*f;
	byte		buf[64], *all;
	size_t		size;

	CHECK (file != NULL);
	if (!file)
		return;
	fputs ("JUNKJUNpayload bytesTRAIL", file);
	fclose (file);

	// a pak's entry: 13 bytes from 7
	file = fopen (name, "rb");
	f = VFS_OpenStdio (file, 7, 13);
	CHECK (VFS_Length (f) == 13 && VFS_Status (f) == VFS_ENDED);
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 13 && !memcmp (buf, "payload bytes", 13));
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 0);
	CHECK (VFS_Seek (f, 8) && VFS_Read (f, buf, 3) == 3 && !memcmp (buf, "byt", 3));
	CHECK (!VFS_Seek (f, 14));
	CHECK (VFS_Seek (f, 0));
	all = VFS_ReadAll (f, &size);
	CHECK (all && size == 13 && !memcmp (all, "payload bytes", 14));
	Mem_Free (all);
	VFS_Close (f);

	// to the file's end
	file = fopen (name, "rb");
	f = VFS_OpenStdio (file, 20, -1);
	CHECK (VFS_Length (f) == 5);
	all = VFS_ReadAll (f, &size);
	CHECK (all && size == 5 && !memcmp (all, "TRAIL", 6));
	Mem_Free (all);
	VFS_Close (f);
	remove (name);
}

static void TestGunzipWhole (void)
{
	vfsfile_t	*f = VFS_OpenGunzip (VFS_OpenMemory (gz, gzsize, false));
	byte		*all, buf[100];
	size_t		size;

	// over a file that has all come, all of it unpacks at once
	CHECK (VFS_Status (f) == VFS_ENDED);
	CHECK (VFS_Length (f) == DATASIZE);
	all = VFS_ReadAll (f, &size);
	CHECK (all && size == DATASIZE && !memcmp (all, data, DATASIZE));
	Mem_Free (all);
	CHECK (VFS_Seek (f, 12345) && VFS_Read (f, buf, 100) == 100 && !memcmp (buf, data + 12345, 100));
	CHECK (VFS_Tell (f) == 12445);
	CHECK (!VFS_Seek (f, DATASIZE + 1));
	CHECK (VFS_Seek (f, DATASIZE) && VFS_Read (f, buf, 100) == 0);
	VFS_Close (f);
}

static void TestGunzipPieces (void)
{
	feeder_t	*d = Feeder (gz, gzsize);
	vfsfile_t	*f = VFS_OpenGunzip (&d->vfs);
	byte		*out = Mem_Alloc (DATASIZE + 1);
	size_t		have = 0;
	bool		live = true;

	// a byte at first: not yet known for a gzip
	d->fed = 1;
	CHECK (VFS_Read (f, out, 10) == 0 && VFS_Status (f) == VFS_LIVE);
	while (d->fed < d->size)
	{
		d->fed = d->fed + 5000 < d->size ? d->fed + 5000 : d->size;
		have += ReadOut (f, out + have, DATASIZE + 1 - have, 3000);
		if (d->fed < d->size)
			live &= VFS_Status (f) == VFS_LIVE && VFS_Length (f) == -1;
	}
	CHECK (live);
	CHECK (VFS_Status (f) == VFS_ENDED && VFS_Length (f) == DATASIZE);
	CHECK (have == DATASIZE && !memcmp (out, data, DATASIZE));
	// what was unpacked is kept, to seek back in
	CHECK (VFS_Seek (f, 0) && VFS_Read (f, out, 10) == 10 && !memcmp (out, data, 10));
	Mem_Free (out);
	VFS_Close (f);
}

static void TestGunzipCut (void)
{
	vfsfile_t	*f = VFS_OpenGunzip (VFS_OpenMemory (gz, gzsize / 2, false));
	byte		*out = Mem_Alloc (DATASIZE);
	size_t		size, have;

	CHECK (VFS_Status (f) == VFS_FAILED);
	CHECK (*VFS_Error (f) != 0);
	CHECK (VFS_ReadAll (f, &size) == NULL);
	// what it unpacked before it stopped can still be read
	have = ReadOut (f, out, DATASIZE, 65536);
	CHECK (have > 0 && have < DATASIZE && !memcmp (out, data, have));
	Mem_Free (out);
	VFS_Close (f);
}

static void TestGunzipFails (void)
{
	feeder_t	*d = Feeder (gz, gzsize);
	vfsfile_t	*f = VFS_OpenGunzip (&d->vfs);
	byte		buf[4096];

	d->fed = gzsize / 3;
	d->fails = true;
	while (VFS_Read (f, buf, sizeof(buf)) > 0)
		;
	CHECK (VFS_Status (f) == VFS_FAILED);
	CHECK (!strcmp (VFS_Error (f), "the feeder failed"));
	VFS_Close (f);
}

static void TestPassThrough (void)
{
	byte		text[] = "not a gzip at all", buf[32];
	feeder_t	*d;
	vfsfile_t	*f = VFS_OpenGunzip (VFS_OpenMemory (text, 17, false));

	CHECK (VFS_Status (f) == VFS_ENDED && VFS_Length (f) == 17);
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 17 && !memcmp (buf, text, 17));
	CHECK (VFS_Tell (f) == 17);
	CHECK (VFS_Seek (f, 4) && VFS_Read (f, buf, 3) == 3 && !memcmp (buf, "a g", 3));
	VFS_Close (f);

	// empty
	f = VFS_OpenGunzip (VFS_OpenMemory (text, 0, false));
	CHECK (VFS_Status (f) == VFS_ENDED && VFS_Length (f) == 0 && VFS_Read (f, buf, 1) == 0);
	VFS_Close (f);

	// one that comes in pieces: it waits for two bytes, then reads as it is
	d = Feeder (text, 17);
	f = VFS_OpenGunzip (&d->vfs);
	d->fed = 1;
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 0 && VFS_Status (f) == VFS_LIVE);
	d->fed = 10;
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 10 && !memcmp (buf, text, 10) && VFS_Status (f) == VFS_LIVE);
	d->fed = 17;
	CHECK (VFS_Read (f, buf, sizeof(buf)) == 7 && VFS_Status (f) == VFS_ENDED);
	VFS_Close (f);
}

int main (void)
{
	MakeData ();
	TestMemory ();
	TestStdio ();
	TestGunzipWhole ();
	TestGunzipPieces ();
	TestGunzipCut ();
	TestGunzipFails ();
	TestPassThrough ();
	Mem_Free (data);
	Mem_Free (gz);
	if (failures)
	{
		printf ("%d failed\n", failures);
		return 1;
	}
	printf ("vfs: all passed\n");
	return 0;
}
