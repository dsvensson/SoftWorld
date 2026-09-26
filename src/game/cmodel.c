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
// cmodel.c -- collision model: the clipping hulls, leafs and visibility of a map

#include "cmodel.h"
#include "arena.h"
#include "fs.h"
#include "md4.h"
#include "mem.h"
#include "print.h"
#include "q_endian.h"
#include "q_string.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>

typedef struct cnode_s
{
	int				contents;		// 0, to tell nodes from leafs
	mplane_t		*plane;
	struct cnode_s	*children[2];	// nodes, or leafs cast to nodes
} cnode_t;

struct cleaf_s
{
	int			contents;			// a negative contents number
	byte		*compressed_vis;	// NULL if everything is visible
	byte		ambient_sound_level[NUM_AMBIENTS];
};

struct cmap_s
{
	char		name[MAX_QPATH];
	unsigned	filesum;			// of the whole file, to notice changes
	unsigned	checksum, checksum2;

	mplane_t	*planes;
	int			numplanes;
	cnode_t		*nodes;
	int			numnodes;
	cleaf_t		*leafs;
	int			numleafs;
	int			numvisleafs;		// leafs 1 .. numvisleafs have visibility
	mclipnode_t	*clipnodes;			// hulls 1 and 2
	int			numclipnodes;
	mclipnode_t	*hull0nodes;		// the nodes as a clipping hull
	byte		*visdata;
	int			vissize;
	char		*entitystring;
	cmodel_t	*cmodels;			// the world, then the inline models
	int			numcmodels;

	int			visbytes;			// size of each visibility buffer
	byte		*novis;				// everything visible
	byte		*pvs;
	byte		*fatpvs;

	arena_t		arena;				// everything of the map
	int			refs;
	struct cmap_s	*next;
};

static cmap_t	*cm_maps;			// the loaded maps

/*
===============================================================================

LOADING

===============================================================================
*/

static cmap_t	*lm;				// the map being loaded
static byte		*cm_base;			// its file
static int		cm_filesize;

/*
=================
CM_Lump

Checks that a lump lies within the file and holds whole elements
=================
*/
static void *CM_Lump (const lump_t *l, size_t elemsize, int *count)
{
	if (l->fileofs < 0 || l->filelen < 0 || l->filelen > cm_filesize - l->fileofs)
		Sys_Error ("CM_LoadMap: lump outside of %s", lm->name);
	if (l->filelen % elemsize)
		Sys_Error ("CM_LoadMap: funny lump size in %s", lm->name);
	*count = (int)(l->filelen / elemsize);
	return cm_base + l->fileofs;
}

static void CM_LoadPlanes (const lump_t *l)
{
	dplane_t	*in;
	mplane_t	*out;
	int			i, j, count, bits;

	in = CM_Lump (l, sizeof(*in), &count);
	out = Arena_Alloc (&lm->arena, (size_t)count * sizeof(*out));
	lm->planes = out;
	lm->numplanes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}
		out->dist = LittleFloat (in->dist);
		out->type = (byte)LittleLong (in->type);
		out->signbits = (byte)bits;
	}
}

static void CM_LoadVisibility (const lump_t *l)
{
	byte	*in;
	int		count;

	in = CM_Lump (l, 1, &count);
	lm->vissize = count;
	if (!count)
		return;
	lm->visdata = Arena_Alloc (&lm->arena, (size_t)count);
	memcpy (lm->visdata, in, (size_t)count);
}

static void CM_LoadLeafs (const lump_t *l)
{
	dleaf_t	*in;
	cleaf_t	*out;
	int		i, j, count, p;

	in = CM_Lump (l, sizeof(*in), &count);
	if (count < 1)
		Sys_Error ("CM_LoadMap: %s has no leafs", lm->name);
	out = Arena_Alloc (&lm->arena, (size_t)count * sizeof(*out));
	lm->leafs = out;
	lm->numleafs = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		out->contents = LittleLong (in->contents);

		p = LittleLong (in->visofs);
		if (lm->visdata && p >= 0 && p < lm->vissize)
			out->compressed_vis = lm->visdata + p;

		for (j=0 ; j<NUM_AMBIENTS ; j++)
			out->ambient_sound_level[j] = in->ambient_level[j];
	}
}

static void CM_LoadNodes (const lump_t *l)
{
	dnode_t	*in;
	cnode_t	*out;
	int		i, j, count, p;

	in = CM_Lump (l, sizeof(*in), &count);
	if (count < 1)
		Sys_Error ("CM_LoadMap: %s has no nodes", lm->name);
	out = Arena_Alloc (&lm->arena, (size_t)count * sizeof(*out));
	lm->nodes = out;
	lm->numnodes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		p = LittleLong (in->planenum);
		if (p < 0 || p >= lm->numplanes)
			Sys_Error ("CM_LoadMap: bad node plane in %s", lm->name);
		out->plane = lm->planes + p;

		for (j=0 ; j<2 ; j++)
		{
			p = LittleShort (in->children[j]);
			if (p >= 0)
			{
				if (p >= count)
					Sys_Error ("CM_LoadMap: bad node child in %s", lm->name);
				out->children[j] = lm->nodes + p;
			}
			else
			{
				p = -1 - p;
				if (p >= lm->numleafs)
					Sys_Error ("CM_LoadMap: bad leaf child in %s", lm->name);
				out->children[j] = (cnode_t *)(lm->leafs + p);
			}
		}
	}
}

static void CM_LoadClipnodes (const lump_t *l)
{
	dclipnode_t	*in;
	mclipnode_t	*out;
	int			i, j, count;

	in = CM_Lump (l, sizeof(*in), &count);
	out = Arena_Alloc (&lm->arena, (size_t)count * sizeof(*out));
	lm->clipnodes = out;
	lm->numclipnodes = count;

	for (i=0 ; i<count ; i++, in++, out++)
	{
		out->planenum = LittleLong (in->planenum);
		if (out->planenum < 0 || out->planenum >= lm->numplanes)
			Sys_Error ("CM_LoadMap: bad clipnode plane in %s", lm->name);
		for (j=0 ; j<2 ; j++)
		{
			out->children[j] = LittleShort (in->children[j]);
			if (out->children[j] >= count)
				Sys_Error ("CM_LoadMap: bad clipnode child in %s", lm->name);
		}
	}
}

/*
=================
CM_MakeHull0

Duplicates the drawing hull structure as a clipping hull
=================
*/
static void CM_MakeHull0 (void)
{
	cnode_t		*in, *child;
	mclipnode_t	*out;
	int			i, j;

	out = Arena_Alloc (&lm->arena, (size_t)lm->numnodes * sizeof(*out));
	lm->hull0nodes = out;

	for (i=0, in=lm->nodes ; i<lm->numnodes ; i++, in++, out++)
	{
		out->planenum = (int)(in->plane - lm->planes);
		for (j=0 ; j<2 ; j++)
		{
			child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = (int)(child - lm->nodes);
		}
	}
}

static void CM_LoadEntities (const lump_t *l)
{
	char	*in;
	int		count;

	in = CM_Lump (l, 1, &count);
	lm->entitystring = Arena_Alloc (&lm->arena, (size_t)count + 1);
	memcpy (lm->entitystring, in, (size_t)count);
}

static const vec3_t	hull0_size[2] = {{0, 0, 0}, {0, 0, 0}};
static const vec3_t	hull1_size[2] = {{-16, -16, -24}, {16, 16, 32}};
static const vec3_t	hull2_size[2] = {{-32, -32, -24}, {32, 32, 64}};

static void CM_SetHull (hull_t *hull, mclipnode_t *clipnodes, int numclipnodes, int headnode,
	const vec3_t size[2])
{
	if (headnode >= numclipnodes)
		Sys_Error ("CM_LoadMap: bad model headnode in %s", lm->name);
	hull->clipnodes = clipnodes;
	hull->planes = lm->planes;
	hull->firstclipnode = headnode;
	hull->lastclipnode = numclipnodes - 1;
	VectorCopy (size[0], hull->clip_mins);
	VectorCopy (size[1], hull->clip_maxs);
}

static void CM_LoadSubmodels (const lump_t *l)
{
	dmodel_t	*in;
	cmodel_t	*out;
	int			i, j, count;

	in = CM_Lump (l, sizeof(*in), &count);
	if (count < 1)
		Sys_Error ("CM_LoadMap: %s has no models", lm->name);
	out = Arena_Alloc (&lm->arena, (size_t)count * sizeof(*out));
	lm->cmodels = out;
	lm->numcmodels = count;

	lm->numvisleafs = LittleLong (in->visleafs);
	if (lm->numvisleafs < 0 || lm->numvisleafs >= lm->numleafs)
		Sys_Error ("CM_LoadMap: bad visleafs in %s", lm->name);

	for (i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
		}
		CM_SetHull (&out->hulls[0], lm->hull0nodes, lm->numnodes, LittleLong (in->headnode[0]), hull0_size);
		CM_SetHull (&out->hulls[1], lm->clipnodes, lm->numclipnodes, LittleLong (in->headnode[1]), hull1_size);
		CM_SetHull (&out->hulls[2], lm->clipnodes, lm->numclipnodes, LittleLong (in->headnode[2]), hull2_size);
	}
}

static void CM_LoadBrushMap (void)
{
	dheader_t	*header;
	unsigned	sum;
	int			i, count;

	if (cm_filesize < (int)sizeof(dheader_t))
		Sys_Error ("CM_LoadMap: %s is too short", lm->name);
	header = (dheader_t *)cm_base;

	i = LittleLong (header->version);
	if (i != BSPVERSION)
		Sys_Error ("CM_LoadMap: %s has wrong version number (%i should be %i)", lm->name, i, BSPVERSION);

	for (i=0 ; i<(int)(sizeof(dheader_t)/4) ; i++)
		((int *)header)[i] = LittleLong (((int *)header)[i]);

	// checksum all of the map, except for entities
	for (i=0 ; i<HEADER_LUMPS ; i++)
	{
		CM_Lump (&header->lumps[i], 1, &count);
		if (i == LUMP_ENTITIES)
			continue;
		sum = LittleLong (Com_BlockChecksum (cm_base + header->lumps[i].fileofs, count));
		lm->checksum ^= sum;
		if (i == LUMP_VISIBILITY || i == LUMP_LEAFS || i == LUMP_NODES)
			continue;
		lm->checksum2 ^= sum;
	}

	CM_LoadPlanes (&header->lumps[LUMP_PLANES]);
	CM_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
	CM_LoadLeafs (&header->lumps[LUMP_LEAFS]);
	CM_LoadNodes (&header->lumps[LUMP_NODES]);
	CM_LoadClipnodes (&header->lumps[LUMP_CLIPNODES]);
	CM_LoadEntities (&header->lumps[LUMP_ENTITIES]);
	CM_MakeHull0 ();
	CM_LoadSubmodels (&header->lumps[LUMP_MODELS]);

	// room for a row of bits for every leaf, read in whole 32 bit words
	lm->visbytes = (((lm->numleafs + 31) >> 3) + 3) & ~3;
	lm->novis = Arena_Alloc (&lm->arena, (size_t)lm->visbytes);
	memset (lm->novis, 0xff, (size_t)lm->visbytes);
	lm->pvs = Arena_Alloc (&lm->arena, (size_t)lm->visbytes);
	lm->fatpvs = Arena_Alloc (&lm->arena, (size_t)lm->visbytes);
}

/*
=================
CM_LoadMap
=================
*/
cmap_t *CM_LoadMap (const char *name, unsigned *checksum, unsigned *checksum2)
{
	cmap_t		*map;
	byte		*buf;
	int			filesize;
	unsigned	filesum;

	buf = FS_LoadFile (name, &filesize);
	if (!buf)
		return NULL;
	filesum = Com_BlockChecksum (buf, filesize);

	// a listen server and its client share a map they both use
	for (map = cm_maps ; map ; map = map->next)
		if (!strcmp (map->name, name) && map->filesum == filesum)
			break;
	if (!map)
	{
		map = Mem_Calloc (1, sizeof(*map));
		Arena_Init (&map->arena, "collision map");
		Q_strncpyz (map->name, name, sizeof(map->name));
		map->filesum = filesum;

		lm = map;
		cm_base = buf;
		cm_filesize = filesize;
		CM_LoadBrushMap ();
		cm_base = NULL;
		lm = NULL;

		map->next = cm_maps;
		cm_maps = map;
	}
	Mem_Free (buf);
	map->refs++;

	if (checksum)
		*checksum = map->checksum;
	if (checksum2)
		*checksum2 = map->checksum2;
	return map;
}

/*
=================
CM_FreeMap
=================
*/
void CM_FreeMap (cmap_t *map)
{
	cmap_t	**link;

	if (--map->refs > 0)
		return;
	for (link = &cm_maps ; *link != map ; link = &(*link)->next)
		;
	*link = map->next;
	Arena_Free (&map->arena);
	Mem_Free (map);
}

cmodel_t *CM_WorldModel (cmap_t *map)
{
	return &map->cmodels[0];
}

cmodel_t *CM_InlineModel (cmap_t *map, const char *name)
{
	int		num;

	if (name[0] != '*' || !map->numcmodels)
		return NULL;
	num = atoi (name + 1);
	if (num < 1 || num >= map->numcmodels)
		return NULL;
	return &map->cmodels[num];
}

int CM_NumInlineModels (const cmap_t *map)
{
	return map->numcmodels;
}

char *CM_EntityString (const cmap_t *map)
{
	return map->entitystring;
}

/*
===============================================================================

HULL BOXES

===============================================================================
*/

static hull_t		box_hull;
static mclipnode_t	box_clipnodes[6];
static mplane_t		box_planes[6];

/*
===================
CM_InitBoxHull

Set up the planes and clipnodes so that the six floats of a bounding box
can just be stored out and get a proper hull_t structure.
===================
*/
static void CM_InitBoxHull (void)
{
	int		i;
	int		side;

	box_hull.clipnodes = box_clipnodes;
	box_hull.planes = box_planes;
	box_hull.firstclipnode = 0;
	box_hull.lastclipnode = 5;

	for (i=0 ; i<6 ; i++)
	{
		box_clipnodes[i].planenum = i;

		side = i&1;

		box_clipnodes[i].children[side] = CONTENTS_EMPTY;
		if (i != 5)
			box_clipnodes[i].children[side^1] = i + 1;
		else
			box_clipnodes[i].children[side^1] = CONTENTS_SOLID;

		box_planes[i].type = (byte)(i>>1);
		box_planes[i].normal[i>>1] = 1;
	}
}

/*
===================
CM_HullForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
hull_t *CM_HullForBox (const vec3_t mins, const vec3_t maxs)
{
	if (!box_hull.clipnodes)
		CM_InitBoxHull ();

	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];

	return &box_hull;
}

/*
===============================================================================

POINT AND LINE TESTING IN HULLS

===============================================================================
*/

/*
==================
CM_HullPointContents
==================
*/
int CM_HullPointContents (const hull_t *hull, int num, const vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Sys_Error ("CM_HullPointContents: bad node number");

		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

// 1/32 epsilon to keep floating point happy
#define	DIST_EPSILON	(0.03125)

/*
==================
CM_RecursiveHullCheck
==================
*/
bool CM_RecursiveHullCheck (const hull_t *hull, int num, float p1f, float p2f,
	const vec3_t p1, const vec3_t p2, trace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;

// check for empty
	if (num < 0)
	{
		if (num != CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;
		return true;		// empty
	}

	if (num < hull->firstclipnode || num > hull->lastclipnode)
		Sys_Error ("CM_RecursiveHullCheck: bad node number");

//
// find the point distances
//
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

	if (t1 >= 0 && t2 >= 0)
		return CM_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if (t1 < 0 && t2 < 0)
		return CM_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 < 0)
		frac = (float)((t1 + DIST_EPSILON)/(t1-t2));
	else
		frac = (float)((t1 - DIST_EPSILON)/(t1-t2));
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

	side = (t1 < 0);

// move up to the node
	if (!CM_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace) )
		return false;

	if (CM_HullPointContents (hull, node->children[side^1], mid) != CONTENTS_SOLID)
	// go past the node
		return CM_RecursiveHullCheck (hull, node->children[side^1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		VectorSubtract (vec3_origin, plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

	while (CM_HullPointContents (hull, hull->firstclipnode, mid) == CONTENTS_SOLID)
	{ // shouldn't really happen, but does occasionally
		frac = (float)(frac - 0.1);
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Con_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f)*frac;
		for (i=0 ; i<3 ; i++)
			mid[i] = p1[i] + frac*(p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}

/*
===============================================================================

LEAFS AND VISIBILITY

===============================================================================
*/

const cleaf_t *CM_PointInLeaf (const cmap_t *map, const vec3_t p)
{
	cnode_t		*node;
	mplane_t	*plane;
	float		d;

	if (!map->numnodes)
		Sys_Error ("CM_PointInLeaf: empty map");

	node = map->nodes;
	while (node->contents >= 0)
	{
		plane = node->plane;
		d = DotProduct (p, plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return (const cleaf_t *)node;
}

int CM_Leafnum (const cmap_t *map, const cleaf_t *leaf)
{
	return (int)(leaf - map->leafs);
}

const byte *CM_LeafAmbientLevels (const cleaf_t *leaf)
{
	return leaf->ambient_sound_level;
}

int CM_NumVisLeafs (const cmap_t *map)
{
	return map->numvisleafs;
}

/*
===================
CM_LeafPVS

Decompresses the leaf's row of the visibility matrix
===================
*/
byte *CM_LeafPVS (cmap_t *map, int leafnum)
{
	byte	*in, *inend, *out, *end;
	int		c;

	if (leafnum <= 0 || leafnum >= map->numleafs || !map->leafs[leafnum].compressed_vis)
		return map->novis;

	in = map->leafs[leafnum].compressed_vis;
	inend = map->visdata + map->vissize;
	out = map->pvs;
	end = map->pvs + ((map->numvisleafs + 7) >> 3);

	while (out < end && in < inend)
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}
		if (in + 1 >= inend)
			break;
		c = in[1];
		in += 2;
		while (c-- > 0 && out < end)
			*out++ = 0;
	}
	memset (out, 0, (size_t)(map->pvs + map->visbytes - out));

	return map->pvs;
}

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static void CM_AddToFatPVS (cmap_t *map, const vec3_t org, const cnode_t *node, int fatbytes)
{
	int			i;
	byte		*pvs;
	mplane_t	*plane;
	float		d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != CONTENTS_SOLID)
			{
				pvs = CM_LeafPVS (map, (int)((const cleaf_t *)node - map->leafs));
				for (i=0 ; i<fatbytes ; i++)
					map->fatpvs[i] |= pvs[i];
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			CM_AddToFatPVS (map, org, node->children[0], fatbytes);
			node = node->children[1];
		}
	}
}

/*
=============
CM_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
byte *CM_FatPVS (cmap_t *map, const vec3_t org)
{
	int		fatbytes;

	fatbytes = (map->numvisleafs+31)>>3;
	memset (map->fatpvs, 0, (size_t)fatbytes);
	CM_AddToFatPVS (map, org, map->nodes, fatbytes);
	return map->fatpvs;
}

typedef struct
{
	const cmap_t	*map;
	const float	*mins, *maxs;
	int			*leafs;
	int			count, maxcount;
} touch_t;

static void CM_FindTouchedLeafs_r (touch_t *touch, const cnode_t *node)
{
	int		sides;

	if (node->contents == CONTENTS_SOLID)
		return;

// add an efrag if the node is a leaf
	if (node->contents < 0)
	{
		if (touch->count == touch->maxcount)
			return;
		touch->leafs[touch->count++] = (int)((const cleaf_t *)node - touch->map->leafs) - 1;
		return;
	}

// NODE_MIXED
	sides = BOX_ON_PLANE_SIDE(touch->mins, touch->maxs, node->plane);

// recurse down the contacted sides
	if (sides & 1)
		CM_FindTouchedLeafs_r (touch, node->children[0]);
	if (sides & 2)
		CM_FindTouchedLeafs_r (touch, node->children[1]);
}

int CM_FindTouchedLeafs (const cmap_t *map, const vec3_t mins, const vec3_t maxs, int *leafs, int maxleafs)
{
	touch_t	touch = {.map = map, .mins = mins, .maxs = maxs, .leafs = leafs, .maxcount = maxleafs};

	if (map->numnodes)
		CM_FindTouchedLeafs_r (&touch, map->nodes);
	return touch.count;
}
