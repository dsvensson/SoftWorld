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
// bspfile.c -- reading BSP29 and BSP2 map files. Maps come from servers, so
// everything is checked; records are copied out, as lumps need not be aligned.

#include "bspfile.h"
#include "mem.h"
#include "q_endian.h"
#include "q_string.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool BSP_Fail (bspfile_t *bsp, const char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vsnprintf (bsp->error, sizeof(bsp->error), fmt, args);
	va_end (args);
	return false;
}

bool BSP_Open (bspfile_t *bsp, const char *name, const byte *data, int size)
{
	dheader_t	header;
	int			i;

	memset (bsp, 0, sizeof(*bsp));
	Q_strncpyz (bsp->name, name, sizeof(bsp->name));
	bsp->data = data;
	bsp->size = size;

	if (size < 4)
		return BSP_Fail (bsp, "too short");
	memcpy (&header, data, size < (int)sizeof(header) ? 4 : sizeof(header));
	bsp->version = LittleLong (header.version);
	if (bsp->version != BSPVERSION && bsp->version != BSPVERSION_BSP2)
	{
		if (bsp->version == 30)
			return BSP_Fail (bsp, "a Half-Life map (version 30), not supported");
		if (!memcmp (data, "IBSP", 4) || !memcmp (data, "QBSP", 4) || !memcmp (data, "RBSP", 4))
			return BSP_Fail (bsp, "a Quake 2 or Quake 3 map (%.4s), not supported", (const char *)data);
		if (!memcmp (data, "2PSB", 4))
			return BSP_Fail (bsp, "a 2PSB map, not supported");
		return BSP_Fail (bsp, "unknown version %i", bsp->version);
	}
	if (size < (int)sizeof(header))
		return BSP_Fail (bsp, "too short");

	for (i = 0 ; i < HEADER_LUMPS ; i++)
	{
		bsp->lumps[i].fileofs = LittleLong (header.lumps[i].fileofs);
		bsp->lumps[i].filelen = LittleLong (header.lumps[i].filelen);
		if (bsp->lumps[i].fileofs < 0 || bsp->lumps[i].filelen < 0
			|| bsp->lumps[i].filelen > size - bsp->lumps[i].fileofs)
			return BSP_Fail (bsp, "lump %i lies outside the file", i);
	}
	return true;
}

bool BSP_Lump (bspfile_t *bsp, int lump, size_t elemsize, const byte **data, int *count)
{
	const lump_t	*l = &bsp->lumps[lump];

	if (l->filelen % elemsize)
		return BSP_Fail (bsp, "lump %i is not a whole number of %zu byte records", lump, elemsize);
	*data = bsp->data + l->fileofs;
	*count = (int)(l->filelen / elemsize);
	return true;
}

static bool BSP_IsBSP2 (const bspfile_t *bsp)
{
	return bsp->version == BSPVERSION_BSP2;
}

// an array of count wide records, never NULL
static void *BSP_Array (int count, size_t size)
{
	return Mem_Calloc (count ? (size_t)count : 1, size);
}

bspnode_t *BSP_Nodes (bspfile_t *bsp, int numleafs, int *count)
{
	const byte	*in;
	bspnode_t	*out;
	int			i, j, n, child, leaf;

	if (!BSP_Lump (bsp, LUMP_NODES, BSP_IsBSP2 (bsp) ? sizeof(dnode_bsp2_t) : sizeof(dnode_t), &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		bspnode_t	*o = &out[i];

		if (BSP_IsBSP2 (bsp))
		{
			dnode_bsp2_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			o->planenum = LittleLong (d.planenum);
			for (j = 0 ; j < 3 ; j++)
			{
				o->mins[j] = LittleFloat (d.mins[j]);
				o->maxs[j] = LittleFloat (d.maxs[j]);
			}
			o->firstface = (unsigned)LittleLong ((int)d.firstface);
			o->numfaces = (unsigned)LittleLong ((int)d.numfaces);
			for (j = 0 ; j < 2 ; j++)
			{
				child = LittleLong (d.children[j]);
				leaf = child < 0 ? -1 - child : -1;
				if (leaf < 0 && (child <= i || child >= n))
					goto badchild;
				if (leaf >= numleafs)
					goto badchild;
				o->children[j] = child;
			}
		}
		else
		{
			dnode_t		d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			o->planenum = LittleLong (d.planenum);
			for (j = 0 ; j < 3 ; j++)
			{
				o->mins[j] = LittleShort (d.mins[j]);
				o->maxs[j] = LittleShort (d.maxs[j]);
			}
			o->firstface = (unsigned short)LittleShort ((short)d.firstface);
			o->numfaces = (unsigned short)LittleShort ((short)d.numfaces);
			// read unsigned: indices up to the node count are nodes, the rest
			// count down from 0xffff as leafs (maps with over 32767 nodes)
			for (j = 0 ; j < 2 ; j++)
			{
				child = (unsigned short)LittleShort (d.children[j]);
				if (child < n)
				{
					if (child <= i)
						goto badchild;
					o->children[j] = child;
				}
				else
				{
					leaf = 0xffff - child;
					if (leaf >= numleafs)
						goto badchild;
					o->children[j] = -1 - leaf;
				}
			}
		}
	}
	*count = n;
	return out;

badchild:
	Mem_Free (out);
	BSP_Fail (bsp, "node %i has a bad child", i);
	return NULL;
}

bspclipnode_t *BSP_Clipnodes (bspfile_t *bsp, int *count)
{
	const byte		*in;
	bspclipnode_t	*out;
	int				i, j, n, child;

	if (!BSP_Lump (bsp, LUMP_CLIPNODES, BSP_IsBSP2 (bsp) ? sizeof(dclipnode_bsp2_t) : sizeof(dclipnode_t), &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		if (BSP_IsBSP2 (bsp))
		{
			dclipnode_bsp2_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].planenum = LittleLong (d.planenum);
			for (j = 0 ; j < 2 ; j++)
				out[i].children[j] = LittleLong (d.children[j]);
		}
		else
		{
			dclipnode_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].planenum = LittleLong (d.planenum);
			// unsigned, as for nodes: past the clipnode count they're contents
			for (j = 0 ; j < 2 ; j++)
			{
				child = (unsigned short)LittleShort (d.children[j]);
				out[i].children[j] = child < n ? child : child - 0x10000;
			}
		}
		for (j = 0 ; j < 2 ; j++)
		{
			child = out[i].children[j];
			if (child >= 0 && (child <= i || child >= n))
			{
				Mem_Free (out);
				BSP_Fail (bsp, "clipnode %i has a bad child", i);
				return NULL;
			}
		}
	}
	*count = n;
	return out;
}

bspedge_t *BSP_Edges (bspfile_t *bsp, int *count)
{
	const byte	*in;
	bspedge_t	*out;
	int			i, n;

	if (!BSP_Lump (bsp, LUMP_EDGES, BSP_IsBSP2 (bsp) ? sizeof(dedge_bsp2_t) : sizeof(dedge_t), &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		if (BSP_IsBSP2 (bsp))
		{
			dedge_bsp2_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].v[0] = (unsigned)LittleLong ((int)d.v[0]);
			out[i].v[1] = (unsigned)LittleLong ((int)d.v[1]);
		}
		else
		{
			dedge_t		d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].v[0] = (unsigned short)LittleShort ((short)d.v[0]);
			out[i].v[1] = (unsigned short)LittleShort ((short)d.v[1]);
		}
	}
	*count = n;
	return out;
}

bspface_t *BSP_Faces (bspfile_t *bsp, int *count)
{
	const byte	*in;
	bspface_t	*out;
	int			i, n;

	if (!BSP_Lump (bsp, LUMP_FACES, BSP_IsBSP2 (bsp) ? sizeof(dface_bsp2_t) : sizeof(dface_t), &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		if (BSP_IsBSP2 (bsp))
		{
			dface_bsp2_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].planenum = LittleLong (d.planenum);
			out[i].side = LittleLong (d.side);
			out[i].firstedge = LittleLong (d.firstedge);
			out[i].numedges = LittleLong (d.numedges);
			out[i].texinfo = LittleLong (d.texinfo);
			memcpy (out[i].styles, d.styles, sizeof(d.styles));
			out[i].lightofs = LittleLong (d.lightofs);
		}
		else
		{
			dface_t		d;

			// the 16 bit counts and indices read unsigned
			memcpy (&d, in + i * sizeof(d), sizeof(d));
			out[i].planenum = (unsigned short)LittleShort (d.planenum);
			out[i].side = LittleShort (d.side);
			out[i].firstedge = LittleLong (d.firstedge);
			out[i].numedges = (unsigned short)LittleShort (d.numedges);
			out[i].texinfo = (unsigned short)LittleShort (d.texinfo);
			memcpy (out[i].styles, d.styles, sizeof(d.styles));
			out[i].lightofs = LittleLong (d.lightofs);
		}
	}
	*count = n;
	return out;
}

bspleaf_t *BSP_Leafs (bspfile_t *bsp, int *count)
{
	const byte	*in;
	bspleaf_t	*out;
	int			i, j, n;

	if (!BSP_Lump (bsp, LUMP_LEAFS, BSP_IsBSP2 (bsp) ? sizeof(dleaf_bsp2_t) : sizeof(dleaf_t), &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		bspleaf_t	*o = &out[i];

		if (BSP_IsBSP2 (bsp))
		{
			dleaf_bsp2_t	d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			o->contents = LittleLong (d.contents);
			o->visofs = LittleLong (d.visofs);
			for (j = 0 ; j < 3 ; j++)
			{
				o->mins[j] = LittleFloat (d.mins[j]);
				o->maxs[j] = LittleFloat (d.maxs[j]);
			}
			o->firstmarksurface = (unsigned)LittleLong ((int)d.firstmarksurface);
			o->nummarksurfaces = (unsigned)LittleLong ((int)d.nummarksurfaces);
			memcpy (o->ambient_level, d.ambient_level, sizeof(d.ambient_level));
		}
		else
		{
			dleaf_t		d;

			memcpy (&d, in + i * sizeof(d), sizeof(d));
			o->contents = LittleLong (d.contents);
			o->visofs = LittleLong (d.visofs);
			for (j = 0 ; j < 3 ; j++)
			{
				o->mins[j] = LittleShort (d.mins[j]);
				o->maxs[j] = LittleShort (d.maxs[j]);
			}
			o->firstmarksurface = (unsigned short)LittleShort ((short)d.firstmarksurface);
			o->nummarksurfaces = (unsigned short)LittleShort ((short)d.nummarksurfaces);
			memcpy (o->ambient_level, d.ambient_level, sizeof(d.ambient_level));
		}
	}
	*count = n;
	return out;
}

unsigned *BSP_Marksurfaces (bspfile_t *bsp, int *count)
{
	const byte	*in;
	unsigned	*out;
	int			i, n;

	if (!BSP_Lump (bsp, LUMP_MARKSURFACES, BSP_IsBSP2 (bsp) ? 4 : 2, &in, &n))
		return NULL;
	out = BSP_Array (n, sizeof(*out));

	for (i = 0 ; i < n ; i++)
	{
		if (BSP_IsBSP2 (bsp))
		{
			int		d;

			memcpy (&d, in + i * 4, 4);
			out[i] = (unsigned)LittleLong (d);
		}
		else
		{
			short	d;

			memcpy (&d, in + i * 2, 2);
			out[i] = (unsigned short)LittleShort (d);
		}
	}
	*count = n;
	return out;
}

const byte *BSP_FindBSPXLump (const bspfile_t *bsp, const char *name, int *size)
{
	int		i, end, count, ofs, len;
	char	entry[24];

	end = 0;
	for (i = 0 ; i < HEADER_LUMPS ; i++)
		if (bsp->lumps[i].fileofs + bsp->lumps[i].filelen > end)
			end = bsp->lumps[i].fileofs + bsp->lumps[i].filelen;
	end = (end + 3) & ~3;
	if (end > bsp->size - 8 || memcmp (bsp->data + end, "BSPX", 4))
		return NULL;

	memcpy (&count, bsp->data + end + 4, 4);
	count = LittleLong (count);
	if (count < 0 || count > (bsp->size - end - 8) / 32)
		return NULL;
	for (i = 0 ; i < count ; i++)
	{
		const byte	*e = bsp->data + end + 8 + i * 32;

		memcpy (entry, e, sizeof(entry));
		if (strncmp (entry, name, sizeof(entry)))
			continue;
		memcpy (&ofs, e + 24, 4);
		memcpy (&len, e + 28, 4);
		ofs = LittleLong (ofs);
		len = LittleLong (len);
		if (ofs < 0 || len < 0 || len > bsp->size - ofs)
			return NULL;
		*size = len;
		return bsp->data + ofs;
	}
	return NULL;
}
