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
// test_bspload.c -- loads every map under a directory (the argument, else the
// SW_BASEDIR environment variable; neither is a skipped test), loose and
// inside pak files, as a collision map and as a render model. BSP29 and BSP2
// maps must load; anything else must be refused with a reason, never crash.

#include "bspfile.h"
#include "cmodel.h"
#include "mem.h"
#include "model.h"
#include "q_endian.h"
#include "q_string.h"
#include "render.h"
#include "sys.h"
#include "vid.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static int	loaded, refused, failures;

//
// the platform, as far as the loaders need it
//

void Sys_Printf (char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
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

// the rest is linked in but never called while loading maps
int Sys_FileTime (char *path) { (void)path; return -1; }
void Sys_mkdir (char *path) { (void)path; }
double Sys_DoubleTime (void) { return 0; }
void *Sys_ReserveMemory (size_t size) { return calloc (1, size); }
void Sys_CommitMemory (void *base, size_t size) { (void)base; (void)size; }
int Sys_NumCores (void) { return 1; }
void Sys_SetWorkers (int workers) { (void)workers; }
void Sys_Parallel (int count, void (*job) (void *ctx, int index), void *ctx)
{
	for (int i = 0 ; i < count ; i++)
		job (ctx, i);
}
viddef_t vid;
void VID_Update (void) { }

//
// the maps
//

static bool IsQuakeMap (const byte *buf, int size)
{
	int		version;

	if (size < 4)
		return false;
	memcpy (&version, buf, 4);
	version = LittleLong (version);
	return version == BSPVERSION || version == BSPVERSION_BSP2;
}

static void TestMap (const char *name, byte *buf, int size)
{
	static model_t	mod;
	cmap_t			*map;
	bool			quake, cm_ok, mod_ok;

	quake = IsQuakeMap (buf, size);

	map = CM_LoadMapBuffer (name, buf, size, NULL, NULL);
	cm_ok = map != NULL;
	if (map)
		CM_FreeMap (map);

	memset (&mod, 0, sizeof(mod));
	snprintf (mod.name, sizeof(mod.name), "%s", name);
	mod_ok = Mod_LoadFromBuffer (&mod, buf, size);
	if (mod_ok)
		Mod_Unload (&mod);

	if (quake && cm_ok && mod_ok)
		loaded++;
	else if (!quake && !cm_ok && !mod_ok)
		refused++;
	else
	{
		failures++;
		printf ("FAILED %s: %s map, collision %s, render %s\n", name, quake ? "a Quake" : "not a Quake",
			cm_ok ? "loaded" : "refused", mod_ok ? "loaded" : "refused");
	}
}

static byte *ReadFile (const char *path, int *size)
{
	FILE	*f;
	byte	*buf;
	long	len;

	f = fopen (path, "rb");
	if (!f)
		return NULL;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	buf = Mem_Alloc ((size_t)len + 1);
	if (fread (buf, 1, (size_t)len, f) != (size_t)len)
		len = 0;
	fclose (f);
	*size = (int)len;
	return buf;
}

// the maps/*.bsp in a pak file
static void TestPak (const char *path)
{
	byte	*pak, *map;
	int		size, dirofs, dirlen, i, ofs, len;
	char	name[57], full[512];

	pak = ReadFile (path, &size);
	if (!pak)
		return;
	if (size < 12 || memcmp (pak, "PACK", 4))
	{
		Mem_Free (pak);
		return;
	}
	memcpy (&dirofs, pak + 4, 4);
	memcpy (&dirlen, pak + 8, 4);
	dirofs = LittleLong (dirofs);
	dirlen = LittleLong (dirlen);
	if (dirofs < 0 || dirlen < 0 || dirlen > size - dirofs)
	{
		Mem_Free (pak);
		return;
	}
	for (i = 0 ; i < dirlen / 64 ; i++)
	{
		const byte	*entry = pak + dirofs + i * 64;

		memcpy (name, entry, 56);
		name[56] = 0;
		memcpy (&ofs, entry + 56, 4);
		memcpy (&len, entry + 60, 4);
		ofs = LittleLong (ofs);
		len = LittleLong (len);
		if (Q_strncasecmp (name, "maps/", 5) || strlen (name) < 4 || Q_strcasecmp (name + strlen (name) - 4, ".bsp"))
			continue;
		if (ofs < 0 || len < 0 || len > size - ofs)
			continue;
		// a copy of its own, so reading past a map's end is caught
		map = Mem_Alloc ((size_t)len + 1);
		memcpy (map, pak + ofs, (size_t)len);
		snprintf (full, sizeof(full), "%s:%s", path, name);
		TestMap (full, map, len);
		Mem_Free (map);
	}
	Mem_Free (pak);
}

static void TestDirectory (const char *dir);

// an entry of a directory: a directory to go into, or a map or pak to load
static void TestEntry (const char *path, const char *name, bool isdir)
{
	size_t	n = strlen (name);
	byte	*buf;
	int		size;

	if (isdir)
		TestDirectory (path);
	else if (n > 4 && !Q_strcasecmp (name + n - 4, ".bsp"))
	{
		buf = ReadFile (path, &size);
		if (buf)
		{
			TestMap (path, buf, size);
			Mem_Free (buf);
		}
	}
	else if (n > 4 && !Q_strcasecmp (name + n - 4, ".pak"))
		TestPak (path);
}

#ifdef _WIN32
static void TestDirectory (const char *dir)
{
	struct _finddata64i32_t	fd;
	intptr_t				h;
	char					pattern[512], path[512];

	snprintf (pattern, sizeof(pattern), "%s/*", dir);
	h = _findfirst64i32 (pattern, &fd);
	if (h == -1)
		return;
	do
	{
		if (!strcmp (fd.name, ".") || !strcmp (fd.name, ".."))
			continue;
		snprintf (path, sizeof(path), "%s/%s", dir, fd.name);
		TestEntry (path, fd.name, (fd.attrib & _A_SUBDIR) != 0);
	} while (_findnext64i32 (h, &fd) == 0);
	_findclose (h);
}
#else
static void TestDirectory (const char *dir)
{
	DIR				*d;
	struct dirent	*e;
	struct stat		st;
	char			path[512];

	d = opendir (dir);
	if (!d)
		return;
	while ((e = readdir (d)))
	{
		if (!strcmp (e->d_name, ".") || !strcmp (e->d_name, ".."))
			continue;
		snprintf (path, sizeof(path), "%s/%s", dir, e->d_name);
		if (!stat (path, &st))
			TestEntry (path, e->d_name, S_ISDIR (st.st_mode));
	}
	closedir (d);
}
#endif

#define	SKIPPED		77		// ctest's SKIP_RETURN_CODE

int main (int argc, char **argv)
{
	const char	*root = argc > 1 ? argv[1] : getenv ("SW_BASEDIR");

	if (!root || !*root)
	{
		printf ("no maps: give a directory, or set SW_BASEDIR\n");
		return SKIPPED;
	}
	R_InitTextures ();	// the checkerboard for textures a map lacks
	TestDirectory (root);
	printf ("%d maps loaded, %d other files refused, %d failures\n", loaded, refused, failures);
	return failures || !loaded;
}
