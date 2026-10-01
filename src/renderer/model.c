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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "r_local.h"

static model_t	*loadmodel;

static void Mod_LoadSpriteModel (model_t *mod, void *buffer);
static bool Mod_LoadBrushModel (model_t *mod, byte *buffer, int size);
static void Mod_LoadAliasModel (model_t *mod, void *buffer);
static model_t *Mod_LoadModel (model_t *mod, bool crash);

static model_t	**mod_known;		// every model ever named; entries never move
static int		mod_numknown, mod_maxknown;

static vmarray_t	mod_scratch;		// contiguous working memory for building alias models
static size_t		mod_scratch_used;

/*
===============
Mod_Alloc

Zeroed memory owned by the model being loaded.
===============
*/
static void *Mod_Alloc (size_t size)
{
	return Arena_Alloc (loadmodel->arena, size);
}

/*
===============
Mod_ScratchAlloc

Zeroed, contiguous working memory: alias models refer to their parts by offset,
so they are built in one block and then copied to the model.
===============
*/
static void *Mod_ScratchAlloc (size_t size)
{
	void	*p;

	size = (size + 15) & ~(size_t)15;
	p = VMArray_Reserve (&mod_scratch, mod_scratch_used, size);
	memset (p, 0, size);
	mod_scratch_used += size;
	return p;
}

/*
===============
Mod_FreeData
===============
*/
static void Mod_FreeData (model_t *mod)
{
	if (mod->arena)
	{
		Arena_Free (mod->arena);
		Mem_Free (mod->arena);
		mod->arena = NULL;
	}
	mod->extradata = NULL;
}

/*
===============
Mod_FlushAll

Forces every model to reload, e.g. after the game directory changed.
===============
*/
static void Mod_FlushAll (void)
{
	int		i;

	for (i = 0 ; i < mod_numknown ; i++)
	{
		Mod_FreeData (mod_known[i]);
		mod_known[i]->needload = true;
	}
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	VMArray_Init (&mod_scratch, "model scratch", 1, 256 * 1024 * 1024);
	FS_AddGamedirCallback (Mod_FlushAll);
}

/*
===============
Mod_Init

Caches the data if needed
===============
*/
void *Mod_Extradata (model_t *mod)
{
	if (!mod->extradata)
		Mod_LoadModel (mod, true);

	if (!mod->extradata)
		Sys_Error ("Mod_Extradata: %s has no data", mod->name);
	return mod->extradata;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;
	
	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}
	
	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis

Runs of zero bytes are a zero and a count; a row that runs off the end of the
lump ends there
===================
*/
static byte *Mod_DecompressVis (byte *in, model_t *model)
{
	byte	*out = model->pvs, *end = model->pvs + model->visbytes;
	byte	*inend = model->visdata + model->vissize;
	int		row, c;

	row = (model->numleafs + 7) >> 3;
	memset (out, 0, (size_t)model->visbytes);
	if (!in)
	{	// no vis info, so make all visible
		memset (out, 0xff, (size_t)row);
		return model->pvs;
	}

	while (out < model->pvs + row && in < inend)
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
		out += c < end - out ? c : end - out;
	}
	return model->pvs;
}

byte *Mod_LeafPVS (mleaf_t *leaf, model_t *model)
{
	if (leaf == model->leafs)
		return model->novis;
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

/*
===================
Mod_ClearAll
===================
*/
void Mod_ClearAll (void)
{
	int		i;
	model_t	*mod;

	for (i = 0 ; i < mod_numknown ; i++)
	{
		mod = mod_known[i];
		if (mod->type != mod_alias)
		{
			Mod_FreeData (mod);
			mod->needload = true;
		}
	}
}

/*
==================
Mod_FindName

==================
*/
static model_t *Mod_FindName (char *modname)
{
	int		i;
	model_t	*mod;

	if (!modname[0])
		Sys_Error ("Mod_ForName: NULL name");

//
// search the currently loaded models
//
	for (i = 0 ; i < mod_numknown ; i++)
		if (!strcmp (mod_known[i]->name, modname))
			return mod_known[i];

	if (mod_numknown == mod_maxknown)
	{
		mod_maxknown = mod_maxknown ? mod_maxknown * 2 : 256;
		mod_known = Mem_Realloc (mod_known, (size_t)mod_maxknown * sizeof(*mod_known));
	}
	mod = Mem_Calloc (1, sizeof(model_t));
	Q_strncpyz (mod->name, modname, sizeof(mod->name));
	mod->needload = true;
	mod_known[mod_numknown++] = mod;

	return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *Mod_LoadModel (model_t *mod, bool crash)
{
	byte	*buf;
	int		size;
	bool	ok;

	if (!mod->needload)
		return mod;

	buf = FS_LoadFile (mod->name, &size);
	if (!buf)
	{
		if (crash)
			Sys_Error ("Mod_NumForName: %s not found", mod->name);
		return NULL;
	}
	ok = Mod_LoadFromBuffer (mod, buf, size);
	Mem_Free (buf);
	if (!ok)
	{
		if (crash)
			Sys_Error ("Mod_NumForName: %s can't be loaded", mod->name);
		return NULL;
	}
	return mod;
}

/*
==================
Mod_LoadFromBuffer
==================
*/
bool Mod_LoadFromBuffer (model_t *mod, byte *buffer, int size)
{
	unsigned	ident = 0;

	Mod_FreeData (mod);
	mod->arena = Mem_Alloc (sizeof(arena_t));
	Arena_Init (mod->arena, mod->name);
	loadmodel = mod;
	mod->needload = false;

	if (size >= 4)
		memcpy (&ident, buffer, 4);
	switch (LittleLong ((int)ident))
	{
	case IDPOLYHEADER:
		Mod_LoadAliasModel (mod, buffer);
		break;

	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buffer);
		break;

	default:
		if (!Mod_LoadBrushModel (mod, buffer, size))
		{
			Mod_Unload (mod);
			return false;
		}
		break;
	}
	return true;
}

/*
==================
Mod_Unload
==================
*/
void Mod_Unload (model_t *mod)
{
	Mod_FreeData (mod);
	mod->needload = true;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName (char *modname, bool crash)
{
	model_t	*mod;

	mod = Mod_FindName (modname);
	
	return Mod_LoadModel (mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static bspfile_t	*mod_bsp;		// the map being loaded

// the reason loading failed; returns false
static bool Mod_Fail (const char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vsnprintf (mod_bsp->error, sizeof(mod_bsp->error), fmt, args);
	va_end (args);
	return false;
}

/*
=================
Mod_LoadTextures

Textures without data in the map, or with data that doesn't fit, become the
checkerboard. The mip levels are copied to follow each other.
=================
*/
static bool Mod_LoadTextures (void)
{
	const byte	*lump;
	int			lumplen, i, j, count, ofs, pos, num, max, altmax;
	size_t		pixels;
	miptex_t	mt;
	texture_t	*tx, *tx2;
	texture_t	*anims[10];
	texture_t	*altanims[10];
	bool		ok;

	loadmodel->textures = NULL;
	loadmodel->numtextures = 0;
	if (!BSP_Lump (mod_bsp, LUMP_TEXTURES, 1, &lump, &lumplen))
		return false;
	if (lumplen < 4)
		return true;
	memcpy (&count, lump, 4);
	count = LittleLong (count);
	if (count < 0 || count > (lumplen - 4) / 4)
		return Mod_Fail ("bad texture directory");

	loadmodel->numtextures = count;
	loadmodel->textures = Mod_Alloc ((size_t)count * sizeof(*loadmodel->textures) + 1);

	for (i=0 ; i<count ; i++)
	{
		memcpy (&ofs, lump + 4 + i * 4, 4);
		ofs = LittleLong (ofs);
		if (ofs < 0 || ofs > lumplen - (int)sizeof(mt))
			continue;		// not in the map
		memcpy (&mt, lump + ofs, sizeof(mt));
		mt.name[sizeof(mt.name) - 1] = 0;
		mt.width = (unsigned)LittleLong ((int)mt.width);
		mt.height = (unsigned)LittleLong ((int)mt.height);
		if (!mt.width || !mt.height || (mt.width & 15) || (mt.height & 15) || mt.width > 4096 || mt.height > 4096)
		{
			Con_DPrintf ("%s: texture %s is %ux%u, not multiples of 16\n", loadmodel->name, mt.name, mt.width, mt.height);
			continue;
		}
		// only the full size image is used: the smaller ones in maps may be
		// missing or wrong (fences with color where they should be cut out)
		mt.offsets[0] = (unsigned)LittleLong ((int)mt.offsets[0]);
		if (!mt.offsets[0] || mt.offsets[0] > (unsigned)(lumplen - ofs)
			|| (size_t)mt.width * mt.height > (size_t)(lumplen - ofs) - mt.offsets[0])
			continue;	// kept outside the map, in a wad

		pixels = (size_t)mt.width * mt.height / 64 * 85;
		tx = Mod_Alloc (sizeof(texture_t) + pixels);
		loadmodel->textures[i] = tx;
		memcpy (tx->name, mt.name, sizeof(tx->name));
		tx->width = mt.width;
		tx->height = mt.height;
		pos = (int)sizeof(texture_t);
		for (j=0 ; j<MIPLEVELS ; j++)
		{
			tx->offsets[j] = (unsigned)pos;
			pos += (int)((mt.width >> j) * (mt.height >> j));
		}
		memcpy ((byte *)tx + tx->offsets[0], lump + ofs + mt.offsets[0], (size_t)mt.width * mt.height);
		R_BuildMips (tx, tx->name[0] == '{');
		R_LoadTextureOverride (tx, loadmodel->name, loadmodel->arena);

		if (!Q_strncmp (tx->name, "sky", 3))
			R_InitSky (tx);
	}

//
// sequence the animations; a broken sequence stays still
//
	for (i=0 ; i<count ; i++)
	{
		tx = loadmodel->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue;	// allready sequenced

	// find the number of frames in the animation
		memset (anims, 0, sizeof(anims));
		memset (altanims, 0, sizeof(altanims));

		max = tx->name[1];
		altmax = 0;
		if (max >= 'a' && max <= 'z')
			max -= 'a' - 'A';
		if (max >= '0' && max <= '9')
		{
			max -= '0';
			altmax = 0;
			anims[max] = tx;
			max++;
		}
		else if (max >= 'A' && max <= 'J')
		{
			altmax = max - 'A';
			max = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
		{
			Con_DPrintf ("%s: bad animating texture %s\n", loadmodel->name, tx->name);
			continue;
		}

		ok = true;
		for (j=i+1 ; j<count ; j++)
		{
			tx2 = loadmodel->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name+2, tx->name+2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num+1 > max)
					max = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num+1 > altmax)
					altmax = num+1;
			}
			else
				ok = false;
		}
		for (j=0 ; j<max ; j++)
			if (!anims[j])
				ok = false;
		for (j=0 ; j<altmax ; j++)
			if (!altanims[j])
				ok = false;
		if (!ok)
		{
			Con_DPrintf ("%s: broken texture animation %s\n", loadmodel->name, tx->name);
			continue;
		}

#define	ANIM_CYCLE	2
	// link them all together
		for (j=0 ; j<max ; j++)
		{
			tx2 = anims[j];
			tx2->anim_total = max * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = anims[ (j+1)%max ];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j=0 ; j<altmax ; j++)
		{
			tx2 = altanims[j];
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j+1) * ANIM_CYCLE;
			tx2->anim_next = altanims[ (j+1)%altmax ];
			if (max)
				tx2->alternate_anims = anims[0];
		}
	}
	return true;
}

/*
=================
Mod_LoadVisibility
=================
*/
static bool Mod_LoadVisibility (void)
{
	const byte	*in;
	int			count;

	loadmodel->visdata = NULL;
	loadmodel->vissize = 0;
	if (!BSP_Lump (mod_bsp, LUMP_VISIBILITY, 1, &in, &count))
		return false;
	if (!count)
		return true;
	loadmodel->visdata = Mod_Alloc ((size_t)count);
	loadmodel->vissize = count;
	memcpy (loadmodel->visdata, in, (size_t)count);
	return true;
}

/*
=================
Mod_LoadEntities
=================
*/
static bool Mod_LoadEntities (void)
{
	const byte	*in;
	int			count;

	if (!BSP_Lump (mod_bsp, LUMP_ENTITIES, 1, &in, &count))
		return false;
	loadmodel->entities = Mod_Alloc ((size_t)count + 1);
	memcpy (loadmodel->entities, in, (size_t)count);
	return true;
}

/*
=================
Mod_LoadVertexes
=================
*/
static bool Mod_LoadVertexes (void)
{
	const byte	*in;
	dvertex_t	d;
	mvertex_t	*out;
	int			i, count;

	if (!BSP_Lump (mod_bsp, LUMP_VERTEXES, sizeof(d), &in, &count))
		return false;
	out = Mod_Alloc ((size_t)count * sizeof(*out) + 1);
	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for (i=0 ; i<count ; i++, out++)
	{
		memcpy (&d, in + i * sizeof(d), sizeof(d));
		out->position[0] = LittleFloat (d.point[0]);
		out->position[1] = LittleFloat (d.point[1]);
		out->position[2] = LittleFloat (d.point[2]);
	}
	return true;
}

/*
=================
Mod_LoadSubmodels
=================
*/
static bool Mod_LoadSubmodels (void)
{
	const byte	*in;
	dmodel_t	*out;
	int			i, j, count;

	if (!BSP_Lump (mod_bsp, LUMP_MODELS, sizeof(*out), &in, &count))
		return false;
	if (count < 1)
		return Mod_Fail ("no models");
	out = Mod_Alloc ((size_t)count * sizeof(*out));
	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for (i=0 ; i<count ; i++, out++)
	{
		memcpy (out, in + i * sizeof(*out), sizeof(*out));
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (out->mins[j]) - 1;
			out->maxs[j] = LittleFloat (out->maxs[j]) + 1;
			out->origin[j] = LittleFloat (out->origin[j]);
		}
		for (j=0 ; j<MAX_MAP_HULLS ; j++)
			out->headnode[j] = LittleLong (out->headnode[j]);
		out->visleafs = LittleLong (out->visleafs);
		out->firstface = LittleLong (out->firstface);
		out->numfaces = LittleLong (out->numfaces);

		if (out->headnode[0] < 0 || out->headnode[0] >= loadmodel->numnodes)
			return Mod_Fail ("model %i has a bad head node", i);
		if (out->firstface < 0 || out->numfaces < 0 || out->numfaces > loadmodel->numsurfaces - out->firstface)
			return Mod_Fail ("model %i has bad faces", i);
		if (out->visleafs < 0 || out->visleafs >= loadmodel->numleafs)
			return Mod_Fail ("model %i has bad visleafs", i);
	}
	return true;
}

/*
=================
Mod_LoadEdges
=================
*/
static bool Mod_LoadEdges (void)
{
	bspedge_t	*in;
	medge_t		*out;
	int			i, count;

	in = BSP_Edges (mod_bsp, &count);
	if (!in)
		return false;
	out = Mod_Alloc ((size_t)(count + 1) * sizeof(*out));
	loadmodel->edges = out;
	loadmodel->numedges = count;

	for (i=0 ; i<count ; i++, out++)
	{
		if (in[i].v[0] >= (unsigned)loadmodel->numvertexes || in[i].v[1] >= (unsigned)loadmodel->numvertexes)
		{
			// edge 0 is never used (surfedges -0 and 0 would be the same),
			// and some compilers leave garbage in it
			if (i)
			{
				Mem_Free (in);
				return Mod_Fail ("edge %i has a bad vertex", i);
			}
			in[i].v[0] = in[i].v[1] = 0;
		}
		out->v[0] = in[i].v[0];
		out->v[1] = in[i].v[1];
	}
	Mem_Free (in);
	return true;
}

/*
=================
Mod_LoadTexinfo
=================
*/
static bool Mod_LoadTexinfo (void)
{
	const byte	*in;
	texinfo_t	d;
	mtexinfo_t	*out;
	int			i, j, count, miptex;
	float		len1, len2;

	if (!BSP_Lump (mod_bsp, LUMP_TEXINFO, sizeof(d), &in, &count))
		return false;
	out = Mod_Alloc ((size_t)count * sizeof(*out) + 1);
	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for (i=0 ; i<count ; i++, out++)
	{
		memcpy (&d, in + i * sizeof(d), sizeof(d));
		for (j=0 ; j<4 ; j++)
		{
			out->vecs[0][j] = LittleFloat (d.vecs[0][j]);
			out->vecs[1][j] = LittleFloat (d.vecs[1][j]);
		}
		len1 = Length (out->vecs[0]);
		len2 = Length (out->vecs[1]);
		len1 = (len1 + len2)/2;
		if (len1 < 0.32)
			out->mipadjust = 4;
		else if (len1 < 0.49)
			out->mipadjust = 3;
		else if (len1 < 0.99)
			out->mipadjust = 2;
		else
			out->mipadjust = 1;

		miptex = LittleLong (d.miptex);
		out->flags = LittleLong (d.flags);
		if (miptex >= 0 && miptex < loadmodel->numtextures && loadmodel->textures[miptex])
			out->texture = loadmodel->textures[miptex];
		else
		{
			out->texture = r_notexture_mip;	// checkerboard texture
			out->flags = 0;
		}
	}
	return true;
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[], and the texture coordinates'
range in mins and maxs; rounded as the light tools round them. And the
surface's bounds, s->minmaxs.
================
*/
static bool CalcSurfaceExtents (msurface_t *s, double mins[2], double maxs[2])
{
	double		val;
	int			i, j, e, bmins[2], bmaxs[2];
	mvertex_t	*v;
	mtexinfo_t	*tex;

	mins[0] = mins[1] = 1e30;
	maxs[0] = maxs[1] = -1e30;
	for (j=0 ; j<3 ; j++)
	{
		s->minmaxs[j] = 1e30f;
		s->minmaxs[3+j] = -1e30f;
	}

	tex = s->texinfo;

	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

		for (j=0 ; j<3 ; j++)
		{
			if (v->position[j] < s->minmaxs[j])
				s->minmaxs[j] = v->position[j];
			if (v->position[j] > s->minmaxs[3+j])
				s->minmaxs[3+j] = v->position[j];
		}

		for (j=0 ; j<2 ; j++)
		{
			// as the light compilers do it: x87 precision, rounded to a
			// float (Quakespasm's fix; lightmaps misalign otherwise)
			val = (float)((double)v->position[0] * tex->vecs[j][0] +
				(double)v->position[1] * tex->vecs[j][1] +
				(double)v->position[2] * tex->vecs[j][2] +
				tex->vecs[j][3]);
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{
		if (!s->numedges || mins[i] < -1e9 || maxs[i] > 1e9)
			return Mod_Fail ("face %i has bad texture coordinates", (int)(s - loadmodel->surfaces));
		bmins[i] = (int)floor (mins[i] / 16);
		bmaxs[i] = (int)ceil (maxs[i] / 16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
		// up to 256 lightmap samples a side
		if (!(tex->flags & TEX_SPECIAL) && s->extents[i] > 255 * 16)
			return Mod_Fail ("face %i is too large", (int)(s - loadmodel->surfaces));
	}
	return true;
}

/*
=================
Mod_LoadFaces
=================
*/
static bool Mod_LoadFaces (void)
{
	bspface_t	*in;
	msurface_t	*out;
	int			i, count, surfnum;
	double		texmins[2], texmaxs[2];
	facelumps_t	lumps;

	in = BSP_Faces (mod_bsp, &count);
	if (!in)
		return false;
	R_FindFaceLumps (mod_bsp, count, &lumps);
	out = Mod_Alloc ((size_t)count * sizeof(*out) + 1);
	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	for (surfnum=0 ; surfnum<count ; surfnum++, out++)
	{
		bspface_t	*f = &in[surfnum];

		if (f->planenum < 0 || f->planenum >= loadmodel->numplanes
			|| f->texinfo < 0 || f->texinfo >= loadmodel->numtexinfo
			|| f->firstedge < 0 || f->numedges < 0 || f->numedges > loadmodel->numsurfedges - f->firstedge)
		{
			Mem_Free (in);
			return Mod_Fail ("face %i is bad", surfnum);
		}
		out->firstedge = f->firstedge;
		out->numedges = f->numedges;
		out->flags = f->side ? SURF_PLANEBACK : 0;
		out->plane = loadmodel->planes + f->planenum;
		out->texinfo = loadmodel->texinfo + f->texinfo;

		if (!CalcSurfaceExtents (out, texmins, texmaxs))
		{
			Mem_Free (in);
			return false;
		}
		R_SetFaceLightmap (loadmodel, out, f, &lumps, surfnum, texmins, texmaxs);

	// set the drawing flags flag

		if (out->texinfo->texture->name[0] == '{')	// fence
			out->flags |= SURF_DRAWFENCE;

		if (!Q_strncmp (out->texinfo->texture->name, "sky", 3))	// sky
		{
			out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
			continue;
		}

		if (!Q_strncmp (out->texinfo->texture->name, "*", 1))		// turbulent
		{
			out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
			if (!Q_strncasecmp (out->texinfo->texture->name, "*lava", 5))
				out->flags |= SURF_LAVA;
			else if (!Q_strncasecmp (out->texinfo->texture->name, "*slime", 6))
				out->flags |= SURF_SLIME;
			else if (!Q_strncasecmp (out->texinfo->texture->name, "*tele", 5))
				out->flags |= SURF_TELE;
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			continue;
		}
	}
	Mem_Free (in);
	return true;
}

/*
=================
Mod_SetParents

Links the nodes and leafs of the world's tree, from node 0, to their parents.
Only the world's: the trees of inline models may share leafs with it, and
R_MarkLeaves climbs from a visible leaf through the world's nodes.
=================
*/
static void Mod_SetParents (void)
{
	mnode_t		**stack, *node;
	int			sp, j;

	stack = Mem_Alloc ((size_t)loadmodel->numnodes * sizeof(*stack));
	sp = 0;
	stack[sp++] = loadmodel->nodes;
	loadmodel->nodes->parent = NULL;
	while (sp)
	{
		node = stack[--sp];
		for (j=0 ; j<2 ; j++)
		{
			if (node->children[j]->contents < 0)
				node->children[j]->parent = node;		// a leaf
			else if (!node->children[j]->parent)
			{	// each node once, even in a map that shares them
				node->children[j]->parent = node;
				stack[sp++] = node->children[j];
			}
		}
	}
	Mem_Free (stack);
}

/*
=================
Mod_LoadNodes
=================
*/
static bool Mod_LoadNodes (void)
{
	bspnode_t	*in;
	mnode_t		*out;
	int			i, j, count, p;

	in = BSP_Nodes (mod_bsp, loadmodel->numleafs, &count);
	if (!in)
		return false;
	if (count < 1)
	{
		Mem_Free (in);
		return Mod_Fail ("no nodes");
	}
	out = Mod_Alloc ((size_t)count * sizeof(*out));
	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for (i=0 ; i<count ; i++, out++)
	{
		bspnode_t	*n = &in[i];

		if (n->planenum < 0 || n->planenum >= loadmodel->numplanes
			|| n->numfaces > (unsigned)loadmodel->numsurfaces || n->firstface > (unsigned)loadmodel->numsurfaces - n->numfaces)
		{
			Mem_Free (in);
			return Mod_Fail ("node %i is bad", i);
		}
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = n->mins[j];
			out->minmaxs[3+j] = n->maxs[j];
		}
		out->plane = loadmodel->planes + n->planenum;
		out->firstsurface = n->firstface;
		out->numsurfaces = n->numfaces;

		for (j=0 ; j<2 ; j++)
		{
			p = n->children[j];
			if (p >= 0)
				out->children[j] = loadmodel->nodes + p;
			else
				out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
		}
	}
	Mem_Free (in);
	Mod_SetParents ();
	return true;
}

/*
=================
Mod_LoadLeafs
=================
*/
static bool Mod_LoadLeafs (void)
{
	bspleaf_t	*in;
	mleaf_t		*out;
	int			i, j, count;

	in = BSP_Leafs (mod_bsp, &count);
	if (!in)
		return false;
	if (count < 1)
	{
		Mem_Free (in);
		return Mod_Fail ("no leafs");
	}
	out = Mod_Alloc ((size_t)count * sizeof(*out));
	loadmodel->leafs = out;
	loadmodel->numleafs = count;
	loadmodel->numloadedleafs = count;

	for (i=0 ; i<count ; i++, out++)
	{
		bspleaf_t	*l = &in[i];

		if (l->nummarksurfaces > (unsigned)loadmodel->nummarksurfaces
			|| l->firstmarksurface > (unsigned)loadmodel->nummarksurfaces - l->nummarksurfaces)
		{
			Mem_Free (in);
			return Mod_Fail ("leaf %i has bad surfaces", i);
		}
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = l->mins[j];
			out->minmaxs[3+j] = l->maxs[j];
		}
		out->contents = l->contents;
		out->firstmarksurface = loadmodel->marksurfaces + l->firstmarksurface;
		out->nummarksurfaces = (int)l->nummarksurfaces;
		if (l->visofs < 0 || l->visofs >= loadmodel->vissize)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = loadmodel->visdata + l->visofs;
		out->efrags = NULL;
		for (j=0 ; j<NUM_AMBIENTS ; j++)
			out->ambient_sound_level[j] = l->ambient_level[j];
	}
	Mem_Free (in);
	return true;
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static bool Mod_LoadMarksurfaces (void)
{
	unsigned	*in;
	msurface_t	**out;
	int			i, count;

	in = BSP_Marksurfaces (mod_bsp, &count);
	if (!in)
		return false;
	out = Mod_Alloc ((size_t)count * sizeof(*out) + 1);
	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for (i=0 ; i<count ; i++)
	{
		if (in[i] >= (unsigned)loadmodel->numsurfaces)
		{
			Mem_Free (in);
			return Mod_Fail ("marksurface %i is bad", i);
		}
		out[i] = loadmodel->surfaces + in[i];
	}
	Mem_Free (in);
	return true;
}

/*
=================
Mod_LoadSurfedges
=================
*/
static bool Mod_LoadSurfedges (void)
{
	const byte	*in;
	int			i, count, *out;

	if (!BSP_Lump (mod_bsp, LUMP_SURFEDGES, 4, &in, &count))
		return false;
	out = Mod_Alloc ((size_t)count * sizeof(*out) + 1);
	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for (i=0 ; i<count ; i++)
	{
		memcpy (&out[i], in + i * 4, 4);
		out[i] = LittleLong (out[i]);
		if (out[i] == INT_MIN || abs (out[i]) >= loadmodel->numedges)
			return Mod_Fail ("surfedge %i is bad", i);
	}
	return true;
}

/*
=================
Mod_LoadPlanes
=================
*/
static bool Mod_LoadPlanes (void)
{
	const byte	*in;
	dplane_t	d;
	mplane_t	*out;
	int			i, j, count, bits;

	if (!BSP_Lump (mod_bsp, LUMP_PLANES, sizeof(d), &in, &count))
		return false;
	out = Mod_Alloc ((size_t)count * 2 * sizeof(*out) + 1);
	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for (i=0 ; i<count ; i++, out++)
	{
		memcpy (&d, in + i * sizeof(d), sizeof(d));
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (d.normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}
		out->dist = LittleFloat (d.dist);
		out->type = (byte)LittleLong (d.type);
		out->signbits = (byte)bits;
	}
	return true;
}

/*
=================
RadiusFromBounds
=================
*/
static float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int		i;
	vec3_t	corner;

	for (i=0 ; i<3 ; i++)
	{
		corner[i] = fabsf(mins[i]) > fabsf(maxs[i]) ? fabsf(mins[i]) : fabsf(maxs[i]);
	}

	return Length (corner);
}

/*
=================
Mod_LoadBrushModel
=================
*/
static bool Mod_LoadBrushModel (model_t *mod, byte *buffer, int size)
{
	bspfile_t	bsp;
	dmodel_t	*bm;
	int			i;
	bool		ok;

	loadmodel->type = mod_brush;
	mod_bsp = &bsp;
	ok = BSP_Open (&bsp, mod->name, buffer, size)
		&& Mod_LoadVertexes ()
		&& Mod_LoadEdges ()
		&& Mod_LoadSurfedges ()
		&& Mod_LoadTextures ();
	if (ok)
		R_LoadLightData (loadmodel, &bsp);
	ok = ok
		&& Mod_LoadPlanes ()
		&& Mod_LoadTexinfo ()
		&& Mod_LoadFaces ()
		&& Mod_LoadMarksurfaces ()
		&& Mod_LoadVisibility ()
		&& Mod_LoadLeafs ()
		&& Mod_LoadNodes ()
		&& Mod_LoadEntities ()
		&& Mod_LoadSubmodels ();
	mod_bsp = NULL;
	if (!ok)
	{
		Con_Printf ("Couldn't load %s: %s\n", mod->name, bsp.error);
		return false;
	}

	// a row of visibility bits for every leaf, in whole 32 bit words
	mod->visbytes = (((mod->numleafs + 31) >> 3) + 3) & ~3;
	mod->novis = Mod_Alloc ((size_t)mod->visbytes);
	memset (mod->novis, 0xff, (size_t)mod->visbytes);
	mod->pvs = Mod_Alloc ((size_t)mod->visbytes);

	mod->numframes = 2;		// regular and alternate animation

//
// set up the submodels (FIXME: this is confusing)
//
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		bm = &mod->submodels[i];

		mod->firstnode = bm->headnode[0];

		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;

		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);
		mod->radius = RadiusFromBounds (mod->mins, mod->maxs);

		mod->numleafs = bm->visleafs;

		if (i < mod->numsubmodels-1)
		{	// duplicate the basic information
			char	subname[16];

			snprintf (subname, sizeof(subname), "*%i", i+1);
			loadmodel = Mod_FindName (subname);
			*loadmodel = *mod;
			loadmodel->arena = NULL;	// the data belongs to the world model
			Q_strncpyz (loadmodel->name, subname, sizeof(loadmodel->name));
			mod = loadmodel;
		}
	}
	return true;
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

/*
=================
Mod_LoadAliasFrame
=================
*/
static void * Mod_LoadAliasFrame (void * pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *framename)
{
	trivertx_t		*pframe, *pinframe;
	int				i, j;
	daliasframe_t	*pdaliasframe;

	pdaliasframe = (daliasframe_t *)pin;

// framename always points at a pheader->frames[].name buffer
	Q_strncpyz (framename, pdaliasframe->name, sizeof(pheader->frames[0].name));

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about
	// endianness
		pbboxmin->v[i] = pdaliasframe->bboxmin.v[i];
		pbboxmax->v[i] = pdaliasframe->bboxmax.v[i];
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);
	pframe = Mod_ScratchAlloc (numv * sizeof(*pframe));

	*pframeindex = (int)((byte *)pframe - (byte *)pheader);

	for (j=0 ; j<numv ; j++)
	{
		int		k;

	// these are all byte values, so no need to deal with endianness
		pframe[j].lightnormalindex = pinframe[j].lightnormalindex;

		for (k=0 ; k<3 ; k++)
		{
			pframe[j].v[k] = pinframe[j].v[k];
		}
	}

	pinframe += numv;

	return (void *)pinframe;
}


/*
=================
Mod_LoadAliasGroup
=================
*/
static void * Mod_LoadAliasGroup (void * pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *framename)
{
	daliasgroup_t		*pingroup;
	maliasgroup_t		*paliasgroup;
	int					i, numframes;
	daliasinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;
	
	pingroup = (daliasgroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	paliasgroup = Mod_ScratchAlloc (sizeof (maliasgroup_t) +
			(numframes - 1) * sizeof (paliasgroup->frames[0]));

	paliasgroup->numframes = numframes;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about endianness
		pbboxmin->v[i] = pingroup->bboxmin.v[i];
		pbboxmax->v[i] = pingroup->bboxmax.v[i];
	}

	*pframeindex = (int)((byte *)paliasgroup - (byte *)pheader);

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	poutintervals = Mod_ScratchAlloc (numframes * sizeof (float));

	paliasgroup->intervals = (int)((byte *)poutintervals - (byte *)pheader);

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadAliasGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadAliasFrame (ptemp,
									&paliasgroup->frames[i].frame,
									numv,
									&paliasgroup->frames[i].bboxmin,
									&paliasgroup->frames[i].bboxmax,
									pheader, framename);
	}

	return ptemp;
}


/*
=================
Mod_LoadAliasSkin
=================
*/
static void * Mod_LoadAliasSkin (void * pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	byte	*pskin, *pinskin;

	pskin = Mod_ScratchAlloc (skinsize);
	pinskin = (byte *)pin;
	*pskinindex = (int)((byte *)pskin - (byte *)pheader);

	Q_memcpy (pskin, pinskin, skinsize);

	pinskin += skinsize;

	return ((void *)pinskin);
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
static void * Mod_LoadAliasSkinGroup (void * pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	daliasskingroup_t		*pinskingroup;
	maliasskingroup_t		*paliasskingroup;
	int						i, numskins;
	daliasskininterval_t	*pinskinintervals;
	float					*poutskinintervals;
	void					*ptemp;

	pinskingroup = (daliasskingroup_t *)pin;

	numskins = LittleLong (pinskingroup->numskins);

	paliasskingroup = Mod_ScratchAlloc (sizeof (maliasskingroup_t) +
			(numskins - 1) * sizeof (paliasskingroup->skindescs[0]));

	paliasskingroup->numskins = numskins;

	*pskinindex = (int)((byte *)paliasskingroup - (byte *)pheader);

	pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

	poutskinintervals = Mod_ScratchAlloc (numskins * sizeof (float));

	paliasskingroup->intervals = (int)((byte *)poutskinintervals - (byte *)pheader);

	for (i=0 ; i<numskins ; i++)
	{
		*poutskinintervals = LittleFloat (pinskinintervals->interval);
		if (*poutskinintervals <= 0)
			Sys_Error ("Mod_LoadAliasSkinGroup: interval<=0");

		poutskinintervals++;
		pinskinintervals++;
	}

	ptemp = (void *)pinskinintervals;

	for (i=0 ; i<numskins ; i++)
	{
		ptemp = Mod_LoadAliasSkin (ptemp,
				&paliasskingroup->skindescs[i].skin, skinsize, pheader);
	}

	return ptemp;
}


/*
=================
Mod_LoadAliasModel
=================
*/
static void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int					i;
	mdl_t				*pmodel, *pinmodel;
	stvert_t			*pstverts, *pinstverts;
	aliashdr_t			*pheader;
	mtriangle_t			*ptri;
	dtriangle_t			*pintriangles;
	int					version, numframes, numskins;
	int					size;
	daliasframetype_t	*pframetype;
	daliasskintype_t	*pskintype;
	maliasskindesc_t	*pskindesc;
	int					skinsize;
	size_t				start, total;
	
	start = mod_scratch_used;

	pinmodel = (mdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size = 	sizeof (aliashdr_t) + (LittleLong (pinmodel->numframes) - 1) *
			 sizeof (pheader->frames[0]) +
			sizeof (mdl_t) +
			LittleLong (pinmodel->numverts) * sizeof (stvert_t) +
			LittleLong (pinmodel->numtris) * sizeof (mtriangle_t);

	pheader = Mod_ScratchAlloc (size);
	pmodel = (mdl_t *) ((byte *)&pheader[1] +
			(LittleLong (pinmodel->numframes) - 1) *
			 sizeof (pheader->frames[0]));
	
	mod->flags = LittleLong (pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
	pmodel->boundingradius = LittleFloat (pinmodel->boundingradius);
	pmodel->numskins = LittleLong (pinmodel->numskins);
	pmodel->skinwidth = LittleLong (pinmodel->skinwidth);
	pmodel->skinheight = LittleLong (pinmodel->skinheight);

	if (pmodel->skinwidth <= 0 || pmodel->skinheight <= 0)
		Sys_Error ("model %s has no skin size", mod->name);

	pmodel->numverts = LittleLong (pinmodel->numverts);

	if (pmodel->numverts <= 0)
		Sys_Error ("model %s has no vertices", mod->name);

	if (pmodel->numverts > MAXALIASVERTS)
		Sys_Error ("model %s has too many vertices", mod->name);

	pmodel->numtris = LittleLong (pinmodel->numtris);

	if (pmodel->numtris <= 0)
		Sys_Error ("model %s has no triangles", mod->name);

	pmodel->numframes = LittleLong (pinmodel->numframes);
	pmodel->size = (float)(LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO);
	mod->synctype = LittleLong (pinmodel->synctype);
	mod->numframes = pmodel->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pmodel->scale[i] = LittleFloat (pinmodel->scale[i]);
		pmodel->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pmodel->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}

	numskins = pmodel->numskins;
	numframes = pmodel->numframes;

	if (pmodel->skinwidth & 0x03)
		Sys_Error ("Mod_LoadAliasModel: skinwidth not multiple of 4");

	pheader->model = (int)((byte *)pmodel - (byte *)pheader);

//
// load the skins
//
	skinsize = pmodel->skinheight * pmodel->skinwidth;

	if (numskins < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d\n", numskins);

	pskintype = (daliasskintype_t *)&pinmodel[1];

	pskindesc = Mod_ScratchAlloc (numskins * sizeof (maliasskindesc_t));

	pheader->skindesc = (int)((byte *)pskindesc - (byte *)pheader);

	for (i=0 ; i<numskins ; i++)
	{
		aliasskintype_t	skintype;

		skintype = LittleLong (pskintype->type);
		pskindesc[i].type = skintype;

		if (skintype == ALIAS_SKIN_SINGLE)
		{
			pskintype = (daliasskintype_t *)
					Mod_LoadAliasSkin (pskintype + 1,
									   &pskindesc[i].skin,
									   skinsize, pheader);
		}
		else
		{
			pskintype = (daliasskintype_t *)
					Mod_LoadAliasSkinGroup (pskintype + 1,
											&pskindesc[i].skin,
											skinsize, pheader);
		}
	}

//
// set base s and t vertices
//
	pstverts = (stvert_t *)&pmodel[1];
	pinstverts = (stvert_t *)pskintype;

	pheader->stverts = (int)((byte *)pstverts - (byte *)pheader);

	for (i=0 ; i<pmodel->numverts ; i++)
	{
		pstverts[i].onseam = LittleLong (pinstverts[i].onseam);
	// put s and t in 16.16 format
		pstverts[i].s = LittleLong (pinstverts[i].s) << 16;
		pstverts[i].t = LittleLong (pinstverts[i].t) << 16;
	}

//
// set up the triangles
//
	ptri = (mtriangle_t *)&pstverts[pmodel->numverts];
	pintriangles = (dtriangle_t *)&pinstverts[pmodel->numverts];

	pheader->triangles = (int)((byte *)ptri - (byte *)pheader);

	for (i=0 ; i<pmodel->numtris ; i++)
	{
		int		j;

		ptri[i].facesfront = LittleLong (pintriangles[i].facesfront);

		for (j=0 ; j<3 ; j++)
		{
			ptri[i].vertindex[j] =
					LittleLong (pintriangles[i].vertindex[j]);
		}
	}

//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of frames: %d\n", numframes);

	pframetype = (daliasframetype_t *)&pintriangles[pmodel->numtris];

	for (i=0 ; i<numframes ; i++)
	{
		aliasframetype_t	frametype;

		frametype = LittleLong (pframetype->type);
		pheader->frames[i].type = frametype;


		if (frametype == ALIAS_SINGLE)
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasFrame (pframetype + 1,
										&pheader->frames[i].frame,
										pmodel->numverts,
										&pheader->frames[i].bboxmin,
										&pheader->frames[i].bboxmax,
										pheader, pheader->frames[i].name);
		}
		else
		{
			pframetype = (daliasframetype_t *)
					Mod_LoadAliasGroup (pframetype + 1,
										&pheader->frames[i].frame,
										pmodel->numverts,
										&pheader->frames[i].bboxmin,
										&pheader->frames[i].bboxmax,
										pheader, pheader->frames[i].name);
		}
	}

	mod->type = mod_alias;

// FIXME: do this right
	mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
	mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

//
// move the complete, relocatable alias model into the model's own memory
//
	total = mod_scratch_used - start;
	mod->extradata = Mod_Alloc (total);
	memcpy (mod->extradata, pheader, total);
	mod_scratch_used = start;
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
static void * Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe)
{
	dspriteframe_t		*pinframe;
	mspriteframe_t		*pspriteframe;
	int					width, height, size, origin[2];

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;

	pspriteframe = Mod_Alloc (sizeof (mspriteframe_t) + size);

	Q_memset (pspriteframe, 0, sizeof (mspriteframe_t) + size);
	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = (float)origin[1];
	pspriteframe->down = (float)(origin[1] - height);
	pspriteframe->left = (float)origin[0];
	pspriteframe->right = (float)(width + origin[0]);

	Q_memcpy (&pspriteframe->pixels[0], (byte *)(pinframe + 1), size);

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
static void * Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int					i, numframes;
	dspriteinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	pspritegroup = Mod_Alloc (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]));

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = Mod_Alloc (numframes * sizeof (float));

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadSpriteGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i]);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
static void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	int					i;
	int					version;
	dsprite_t			*pin;
	msprite_t			*psprite;
	int					numframes;
	int					size;
	dspriteframetype_t	*pframetype;
	
	pin = (dsprite_t *)buffer;

	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		Sys_Error ("%s has wrong version number "
				 "(%i should be %i)", mod->name, version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) +	(numframes - 1) * sizeof (psprite->frames);

	psprite = Mod_Alloc (size);

	mod->extradata = psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	psprite->beamlength = LittleFloat (pin->beamlength);
	mod->synctype = LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = (vec_t)(-psprite->maxwidth/2);
	mod->maxs[0] = mod->maxs[1] = (vec_t)(psprite->maxwidth/2);
	mod->mins[2] = (vec_t)(-psprite->maxheight/2);
	mod->maxs[2] = (vec_t)(psprite->maxheight/2);
	
//
// load the frames
//
	if (numframes < 1)
		Sys_Error ("Mod_LoadSpriteModel: Invalid # of frames: %d\n", numframes);

	mod->numframes = numframes;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t	frametype;

		frametype = LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteFrame (pframetype + 1,
										 &psprite->frames[i].frameptr);
		}
		else
		{
			pframetype = (dspriteframetype_t *)
					Mod_LoadSpriteGroup (pframetype + 1,
										 &psprite->frames[i].frameptr);
		}
	}

	mod->type = mod_sprite;
}

//=============================================================================

