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
// wad.c

#include "r_local.h"
#include "md4.h"

// gfx.wad: the one loaded at startup, and the game directory's over it, as
// FTE takes a mod's (its lumps first; those it lacks are the base's)
typedef struct
{
	byte		*base;
	int			size;
	unsigned	checksum;		// of the file as read
	lumpinfo_t	*lumps;
	int			numlumps;
} wadfile_t;

static wadfile_t	wad_game, wad_start;

void SwapPic (qpic_t *pic);

/*
==================
W_CleanupName

Lowercases name and pads with spaces and a terminating 0 to the length of
lumpinfo_t->name.
Used so lumpname lookups can proceed rapidly by comparing 4 chars at a time
Space padding is so names can be printed nicely in tables.
Can safely be performed in place.
==================
*/
static void W_CleanupName (char *in, char *out)
{
	int		i;
	int		c;
	
	for (i=0 ; i<16 ; i++ )
	{
		c = in[i];
		if (!c)
			break;
			
		if (c >= 'A' && c <= 'Z')
			c += ('a' - 'A');
		out[i] = (char)c;
	}
	
	for ( ; i< 16 ; i++ )
		out[i] = 0;
}



// the lumps of a WAD2 file, checked against its size; false if it isn't one
static bool W_Parse (wadfile_t *w, const char *filename)
{
	const wadinfo_t	*header = (const wadinfo_t *)w->base;
	lumpinfo_t		*lump;
	int				i, ofs;

	if (w->size < (int)sizeof(wadinfo_t) || memcmp (header->identification, "WAD2", 4))
	{
		Con_Printf ("%s isn't a WAD2 file\n", filename);
		return false;
	}
	w->numlumps = LittleLong (header->numlumps);
	ofs = LittleLong (header->infotableofs);
	if (w->numlumps < 0 || ofs < 0 || (int64_t)ofs + (int64_t)w->numlumps * (int64_t)sizeof(lumpinfo_t) > w->size)
	{
		Con_Printf ("%s's lump table is past its end\n", filename);
		return false;
	}
	w->lumps = (lumpinfo_t *)(w->base + ofs);
	for (i=0, lump = w->lumps ; i<w->numlumps ; i++, lump++)
	{
		lump->filepos = LittleLong (lump->filepos);
		lump->size = LittleLong (lump->size);
		lump->name[15] = 0;
		W_CleanupName (lump->name, lump->name);
		if (lump->filepos < 0 || lump->size < 0 || (int64_t)lump->filepos + lump->size > w->size)
		{
			Con_Printf ("%s's %s is past its end\n", filename, lump->name);
			return false;
		}
		if (lump->type == TYP_QPIC)
		{
			if (lump->size < 8)
				lump->type = TYP_NONE;
			else
			{
				SwapPic ((qpic_t *)(w->base + lump->filepos));
				if ((int64_t)((qpic_t *)(w->base + lump->filepos))->width
					* ((qpic_t *)(w->base + lump->filepos))->height > lump->size - 8)
					lump->type = TYP_NONE;	// not a picture the size it says
			}
		}
	}
	return true;
}

/*
====================
W_LoadWadFile

The one at startup; it has to be there
====================
*/
void W_LoadWadFile (char *filename)
{
	wad_start.base = FS_LoadFile (filename, &wad_start.size);
	if (!wad_start.base)
		Sys_Error ("W_LoadWadFile: couldn't load %s", filename);
	wad_start.checksum = Com_BlockChecksum (wad_start.base, wad_start.size);
	if (!W_Parse (&wad_start, filename))
		Sys_Error ("W_LoadWadFile: %s isn't usable", filename);
}

/*
====================
W_LoadGameWad

The game directory's gfx.wad, when it has one other than the one at startup
====================
*/
void W_LoadGameWad (void)
{
	Mem_Free (wad_game.base);
	memset (&wad_game, 0, sizeof(wad_game));
	wad_game.base = FS_LoadFile ("gfx.wad", &wad_game.size);
	if (!wad_game.base)
		return;
	wad_game.checksum = Com_BlockChecksum (wad_game.base, wad_game.size);
	if ((wad_game.size == wad_start.size && wad_game.checksum == wad_start.checksum)
		|| !W_Parse (&wad_game, "gfx.wad"))
	{	// the startup one again, or none to use
		Mem_Free (wad_game.base);
		memset (&wad_game, 0, sizeof(wad_game));
	}
}

// a lump by its cleaned name: the game directory's, else the startup one's
static lumpinfo_t *W_FindLump (const char *clean, wadfile_t **from)
{
	wadfile_t	*w;
	int			i, k;

	for (k = 0 ; k < 2 ; k++)
	{
		w = k ? &wad_start : &wad_game;
		for (i=0 ; i<w->numlumps ; i++)
			if (!strcmp (clean, w->lumps[i].name))
			{
				*from = w;
				return &w->lumps[i];
			}
	}
	return NULL;
}

void *W_GetLumpName (char *lumpname)
{
	lumpinfo_t	*lump;
	wadfile_t	*w;
	char		clean[16];

	W_CleanupName (lumpname, clean);
	if (!(lump = W_FindLump (clean, &w)))
		Sys_Error ("W_GetLumpName: %s not found", lumpname);
	return (void *)(w->base + lump->filepos);
}

/*
=============
W_TryGetPic

A picture lump by name, NULL if there is none: for names QuakeC gives
=============
*/
qpic_t *W_TryGetPic (const char *lumpname)
{
	lumpinfo_t	*lump;
	wadfile_t	*w;
	char		in[16], clean[16];

	if (strlen (lumpname) >= sizeof(in))
		return NULL;
	Q_strncpyz (in, lumpname, sizeof(in));
	W_CleanupName (in, clean);
	if (!(lump = W_FindLump (clean, &w)) || lump->type != TYP_QPIC)
		return NULL;
	return (qpic_t *)(w->base + lump->filepos);
}

/*
=============================================================================

automatic byte swapping

=============================================================================
*/

void SwapPic (qpic_t *pic)
{
	pic->width = LittleLong(pic->width);
	pic->height = LittleLong(pic->height);	
}
