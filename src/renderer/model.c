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
#include "vispatch.h"

// the model being loaded, on the thread loading it: the main one, or a
// loader's (Mod_LoadDetached)
static thread_local model_t	*loadmodel;

// the alias or sprite file being read, on the thread loading it: where it
// ends, and why it can't be used
static thread_local const byte	*mod_end;
static thread_local char		mod_error[128];

static bool Mod_LoadSpriteModel (model_t *mod, const void *buffer, int filesize);
static bool Mod_LoadBrushModel (model_t *mod, byte *buffer, int size);
static bool Mod_LoadAliasModel (model_t *mod, const void *buffer, int filesize);
static model_t *Mod_LoadModel (model_t *mod, bool crash);
static void Mod_SetSubmodel (model_t *mod, const dmodel_t *bm);

static model_t	**mod_known;		// every model ever named; entries never move
static int		mod_numknown, mod_maxknown;

static thread_local vmarray_t	mod_scratch;	// contiguous working memory for building alias
static thread_local size_t		mod_scratch_used;	// models, each loading thread's

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
	if (!mod_scratch.base)
		VMArray_Init (&mod_scratch, "model scratch", 1, 256 * 1024 * 1024);
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

Forces every model to reload, e.g. after the game directory changed. The
surface cache's blocks point into the brush models' surfaces, so it forgets
them first.
===============
*/
static void Mod_FlushAll (void)
{
	int		i;

	D_FlushCaches ();
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
	FS_AddWorldCallback (Mod_FlushAll);
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
	int		row;

	if (leaf == model->leafs)
		return model->novis;
	if (model->viswidened && model->leafrow)
	{	// widened across liquids: copied, as the decompressed row is the caller's
		row = model->leafrow[leaf - model->leafs];
		if (row < 0)
			return model->novis;
		memcpy (model->pvs, model->visrows + row, (size_t)model->visbytes);
		return model->pvs;
	}
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

/*
===================
Mod_WidenVis

As it loads, and whenever r_novis 2 or attract mode change their minds
(R_MarkLeaves): widened the first time it is asked to be, and kept
===================
*/
void Mod_WidenVis (model_t *mod, bool widen)
{
	mod->viswidened = widen;
	if (widen && mod->vissource && !mod->visrows)
		BSP_PatchVis (mod->vissource, mod->visdata, mod->vissize, mod->arena, mod->visbytes, &mod->visrows,
			&mod->leafrow);
}

/*
===================
Mod_ClearAll

The brush and sprite models reload; the surface cache forgets the surfaces
first (Mod_FlushAll)
===================
*/
void Mod_ClearAll (void)
{
	int		i;
	model_t	*mod;

	D_FlushCaches ();
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
===================
Mod_ForEachTexture

Each texture of the brush models loaded (a map's submodels, "*1" and on,
share the map's)
===================
*/
void Mod_ForEachTexture (void (*fn) (texture_t *tx))
{
	int		i, j;
	model_t	*mod;

	for (i = 0 ; i < mod_numknown ; i++)
	{
		mod = mod_known[i];
		if (mod->type != mod_brush || mod->needload || mod->name[0] == '*')
			continue;
		for (j = 0 ; j < mod->numtextures ; j++)
			if (mod->textures[j])
				fn (mod->textures[j]);
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
	model_t	*loaded;

	if (!mod->needload)
		return mod;

	buf = FS_LoadFile (mod->name, &size);
	if (!buf)
	{
		if (crash)
			Sys_Error ("Mod_NumForName: %s not found", mod->name);
		return NULL;
	}
	loaded = Mod_LoadDetached (mod->name, buf, size);
	Mem_Free (buf);
	if (!loaded)
	{
		if (crash)
			Sys_Error ("Mod_NumForName: %s can't be loaded", mod->name);
		return NULL;
	}
	return Mod_Install (loaded);
}

/*
==================
Mod_LoadFromBuffer
==================
*/
bool Mod_LoadFromBuffer (model_t *mod, byte *buffer, int size)
{
	unsigned	ident = 0;
	bool		ok;

	Mod_FreeData (mod);
	mod->arena = Mem_Alloc (sizeof(arena_t));
	Arena_Init (mod->arena, mod->name);
	loadmodel = mod;

	if (size >= 4)
		memcpy (&ident, buffer, 4);
	mod_error[0] = 0;
	switch (LittleLong ((int)ident))
	{
	case IDPOLYHEADER:
		ok = Mod_LoadAliasModel (mod, buffer, size);
		break;

	case IDSPRITEHEADER:
		ok = Mod_LoadSpriteModel (mod, buffer, size);
		break;

	default:
		ok = Mod_LoadBrushModel (mod, buffer, size);	// which says why itself
		break;
	}
	loadmodel = NULL;
	if (!ok)
	{
		if (mod_error[0])
			Con_Printf ("Couldn't load %s: %s\n", mod->name, mod_error);
		Mod_Unload (mod);
		return false;
	}
	mod->needload = false;
	return true;
}

/*
==================
Mod_LoadDetached

A model from its file's contents, of no name the renderer knows, on any
thread (a loader's): Mod_Install makes it the named one, or Mod_FreeDetached
frees it. NULL, the reason printed, if it can't be used.
==================
*/
model_t *Mod_LoadDetached (const char *name, byte *buffer, int size)
{
	model_t	*mod = Mem_Calloc (1, sizeof(*mod));

	Q_strncpyz (mod->name, name, sizeof(mod->name));
	if (Mod_LoadFromBuffer (mod, buffer, size))
		return mod;
	Mem_Free (mod);
	return NULL;
}

void Mod_FreeDetached (model_t *mod)
{
	Mod_FreeData (mod);
	Mem_Free (mod);
}

/*
==================
Mod_Install

A detached model (freed) as the one of its name, on the main thread; the
model that name had loaded goes. Its liquids are lit for the light as it is
now (R_LightModelLiquids): it can have changed while the model loaded.
==================
*/
model_t *Mod_Install (model_t *detached)
{
	model_t	*mod = Mod_FindName (detached->name);

	if (mod->type == mod_brush && !mod->needload)
		D_FlushCaches ();	// the surface cache points into its surfaces
	Mod_FreeData (mod);
	*mod = *detached;
	Mem_Free (detached);
	if (mod->arena)
		mod->arena->name = mod->name;
	if (mod->type == mod_brush)
		R_LightModelLiquids (mod);
	return mod;
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
Mod_FindLoaded

The model of that name if it is loaded, NULL if it isn't
==================
*/
model_t *Mod_FindLoaded (const char *modname)
{
	int		i;

	for (i = 0 ; i < mod_numknown ; i++)
		if (!strcmp (mod_known[i]->name, modname))
			return mod_known[i]->needload ? NULL : mod_known[i];
	return NULL;
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

static thread_local bspfile_t	*mod_bsp;		// the map being loaded

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
		R_BuildTexturePixels (tx, loadmodel->arena);
		R_LoadTextureOverride (tx, loadmodel->name, loadmodel->arena);

		if (!Q_strncmp (tx->name, "sky", 3))
			loadmodel->skytexture = tx;	// the sky once it is the world (R_NewMap)
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

		// a sliver whose texture coordinates span nothing one way (newer
		// compilers make them: AD's ad_sepulcher has a cobweb's) has no texels
		// for the surface cache to draw: at most a line of one, left out
		if (!out->extents[0] || !out->extents[1])
			out->flags |= SURF_NOTEXELS;
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
Mod_MarkSharedEdges

The edges two of the world's faces have, one each way: one face's leading
edge on the screen is then the other's trailing edge, and the renderer emits
it once for both. An edge of three faces or more (or of two the same way)
is emitted for each, or a face would be given another's half of it and its
spans would run on to the screen's edge.
=================
*/
static void Mod_MarkSharedEdges (model_t *mod)
{
	const dmodel_t	*world = &mod->submodels[0];
	int				*uses = Mem_Calloc ((size_t)mod->numedges + 1, sizeof(*uses));
	int				*way = Mem_Calloc ((size_t)mod->numedges + 1, sizeof(*way));
	msurface_t		*s;
	int				i, j, e;

	for (i = world->firstface ; i < world->firstface + world->numfaces ; i++)
	{
		s = &mod->surfaces[i];
		for (j = 0 ; j < s->numedges ; j++)
		{
			e = mod->surfedges[s->firstedge + j];
			uses[abs (e)]++;
			way[abs (e)] += e < 0 ? -1 : 1;
		}
	}
	for (i = 0 ; i < mod->numedges ; i++)
		mod->edges[i].shared = uses[i] == 2 && !way[i];
	Mem_Free (uses);
	Mem_Free (way);
}

/*
=================
Mod_LoadBrushModel
=================
*/
static bool Mod_LoadBrushModel (model_t *mod, byte *buffer, int size)
{
	bspfile_t	bsp;
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
	Mod_MarkSharedEdges (mod);

	// a row of visibility bits for every leaf, in whole 32 bit words
	mod->visbytes = (((mod->numleafs + 31) >> 3) + 3) & ~3;
	mod->novis = Mod_Alloc ((size_t)mod->visbytes);
	memset (mod->novis, 0xff, (size_t)mod->visbytes);
	mod->pvs = Mod_Alloc ((size_t)mod->visbytes);
	// seen across liquids its vis treated as opaque, where the loading
	// thread asks (r_novis 2, attract mode), and kept to be later
	mod->vissource = BSP_VisPatchSource (&bsp, mod->arena);
	Mod_WidenVis (mod, BSP_VisPatchWanted ());

	mod->numframes = 2;		// regular and alternate animation

//
// the whole map is the first of its models; the rest are the world's inline
// models once it is the world (Mod_SetWorld)
//
	Mod_SetSubmodel (mod, &mod->submodels[0]);
	return true;
}

/*
=================
Mod_SetSubmodel

A brush model as one of its file's models: where its nodes and surfaces
start, its bounds
=================
*/
static void Mod_SetSubmodel (model_t *mod, const dmodel_t *bm)
{
	mod->firstnode = bm->headnode[0];

	mod->firstmodelsurface = bm->firstface;
	mod->nummodelsurfaces = bm->numfaces;

	VectorCopy (bm->maxs, mod->maxs);
	VectorCopy (bm->mins, mod->mins);
	mod->radius = RadiusFromBounds (mod->mins, mod->maxs);

	mod->numleafs = bm->visleafs;
}

/*
=================
Mod_SetWorld

The world's inline models, "*1" on: copies of it, as its other models,
sharing its data. Only the world's: another map's (a .bsp precached) would
take their names.
=================
*/
void Mod_SetWorld (model_t *world)
{
	model_t	*sub;
	char	subname[16];
	int		i;

	for (i=1 ; i<world->numsubmodels ; i++)
	{
		snprintf (subname, sizeof(subname), "*%i", i);
		sub = Mod_FindName (subname);
		Mod_FreeData (sub);
		*sub = *world;
		sub->arena = NULL;	// the data belongs to the world model
		Q_strncpyz (sub->name, subname, sizeof(sub->name));
		Mod_SetSubmodel (sub, &world->submodels[i]);
	}
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

// the reason the file can't be used; returns false
static bool Mod_FileError (const char *fmt, ...)
{
	va_list	args;

	va_start (args, fmt);
	vsnprintf (mod_error, sizeof(mod_error), fmt, args);
	va_end (args);
	return false;
}

// whether the file holds count things of size bytes at p
static bool Mod_InFile (const void *p, size_t size, size_t count)
{
	if ((const byte *)p > mod_end || (size && count > (size_t)(mod_end - (const byte *)p) / size))
		return Mod_FileError ("it ends early");
	return true;
}

/*
=================
Mod_LoadAliasFrame

The frame at pin; returns what follows it, NULL if the file can't hold it
=================
*/
static const void *Mod_LoadAliasFrame (const void *pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *framename)
{
	trivertx_t			*pframe;
	const trivertx_t	*pinframe;
	int					i, j;
	const daliasframe_t	*pdaliasframe = pin;

	pinframe = (const trivertx_t *)(pdaliasframe + 1);
	if (!Mod_InFile (pdaliasframe, sizeof(*pdaliasframe), 1) || !Mod_InFile (pinframe, sizeof(*pinframe), (size_t)numv))
		return NULL;

// framename always points at a pheader->frames[].name buffer
	Q_strncpyz (framename, pdaliasframe->name, sizeof(pheader->frames[0].name));

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

	// the box is the vertices', not the file's: a model inside the view by its
	// box isn't clipped (R_AliasCheckBBox), and tools write boxes short of
	// the vertices (Copper's groups' are all 0)
	*pbboxmin = *pbboxmax = pframe[0];
	for (j=1 ; j<numv ; j++)
	{
		for (i=0 ; i<3 ; i++)
		{
			if (pbboxmin->v[i] > pframe[j].v[i])
				pbboxmin->v[i] = pframe[j].v[i];
			if (pbboxmax->v[i] < pframe[j].v[i])
				pbboxmax->v[i] = pframe[j].v[i];
		}
	}

	pinframe += numv;

	return pinframe;
}


/*
=================
Mod_LoadAliasGroup
=================
*/
static const void *Mod_LoadAliasGroup (const void *pin, int *pframeindex, int numv,
	trivertx_t *pbboxmin, trivertx_t *pbboxmax, aliashdr_t *pheader, char *framename)
{
	const daliasgroup_t		*pingroup = pin;
	maliasgroup_t			*paliasgroup;
	int						i, j, numframes;
	const daliasinterval_t	*pin_intervals;
	float					*poutintervals;
	const void				*ptemp;

	if (!Mod_InFile (pingroup, sizeof(*pingroup), 1))
		return NULL;
	numframes = LittleLong (pingroup->numframes);
	if (numframes < 1)
	{
		Mod_FileError ("a frame group has no frames");
		return NULL;
	}
	pin_intervals = (const daliasinterval_t *)(pingroup + 1);
	if (!Mod_InFile (pin_intervals, sizeof(*pin_intervals), (size_t)numframes))
		return NULL;

	paliasgroup = Mod_ScratchAlloc (sizeof (maliasgroup_t) +
			(numframes - 1) * sizeof (paliasgroup->frames[0]));

	paliasgroup->numframes = numframes;

	*pframeindex = (int)((byte *)paliasgroup - (byte *)pheader);

	poutintervals = Mod_ScratchAlloc (numframes * sizeof (float));

	paliasgroup->intervals = (int)((byte *)poutintervals - (byte *)pheader);

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
		{
			Mod_FileError ("a frame group's interval is %g", *poutintervals);
			return NULL;
		}

		poutintervals++;
		pin_intervals++;
	}

	ptemp = pin_intervals;

	for (i=0 ; i<numframes && ptemp ; i++)
	{
		ptemp = Mod_LoadAliasFrame (ptemp,
									&paliasgroup->frames[i].frame,
									numv,
									&paliasgroup->frames[i].bboxmin,
									&paliasgroup->frames[i].bboxmax,
									pheader, framename);
	}
	if (!ptemp)
		return NULL;

	// the group's box holds its frames' (Mod_LoadAliasFrame), not the file's
	*pbboxmin = paliasgroup->frames[0].bboxmin;
	*pbboxmax = paliasgroup->frames[0].bboxmax;
	for (i=1 ; i<numframes ; i++)
	{
		for (j=0 ; j<3 ; j++)
		{
			if (pbboxmin->v[j] > paliasgroup->frames[i].bboxmin.v[j])
				pbboxmin->v[j] = paliasgroup->frames[i].bboxmin.v[j];
			if (pbboxmax->v[j] < paliasgroup->frames[i].bboxmax.v[j])
				pbboxmax->v[j] = paliasgroup->frames[i].bboxmax.v[j];
		}
	}

	return ptemp;
}


/*
=================
Mod_LoadAliasSkin
=================
*/
static const void *Mod_LoadAliasSkin (const void *pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	byte		*pskin;
	const byte	*pinskin = pin;

	if (!Mod_InFile (pinskin, 1, (size_t)skinsize))
		return NULL;
	pskin = Mod_ScratchAlloc (skinsize);
	*pskinindex = (int)((byte *)pskin - (byte *)pheader);

	Q_memcpy (pskin, pinskin, skinsize);

	pinskin += skinsize;

	return pinskin;
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
static const void *Mod_LoadAliasSkinGroup (const void *pin, int *pskinindex, int skinsize,
	aliashdr_t *pheader)
{
	const daliasskingroup_t		*pinskingroup = pin;
	maliasskingroup_t			*paliasskingroup;
	int							i, numskins;
	const daliasskininterval_t	*pinskinintervals;
	float						*poutskinintervals;
	const void					*ptemp;

	if (!Mod_InFile (pinskingroup, sizeof(*pinskingroup), 1))
		return NULL;
	numskins = LittleLong (pinskingroup->numskins);
	if (numskins < 1)
	{
		Mod_FileError ("a skin group has no skins");
		return NULL;
	}
	pinskinintervals = (const daliasskininterval_t *)(pinskingroup + 1);
	if (!Mod_InFile (pinskinintervals, sizeof(*pinskinintervals), (size_t)numskins))
		return NULL;

	paliasskingroup = Mod_ScratchAlloc (sizeof (maliasskingroup_t) +
			(numskins - 1) * sizeof (paliasskingroup->skindescs[0]));

	paliasskingroup->numskins = numskins;

	*pskinindex = (int)((byte *)paliasskingroup - (byte *)pheader);

	poutskinintervals = Mod_ScratchAlloc (numskins * sizeof (float));

	paliasskingroup->intervals = (int)((byte *)poutskinintervals - (byte *)pheader);

	for (i=0 ; i<numskins ; i++)
	{
		*poutskinintervals = LittleFloat (pinskinintervals->interval);
		if (*poutskinintervals <= 0)
		{
			Mod_FileError ("a skin group's interval is %g", *poutskinintervals);
			return NULL;
		}

		poutskinintervals++;
		pinskinintervals++;
	}

	ptemp = pinskinintervals;

	for (i=0 ; i<numskins && ptemp ; i++)
	{
		ptemp = Mod_LoadAliasSkin (ptemp,
				&paliasskingroup->skindescs[i].skin, skinsize, pheader);
	}

	return ptemp;
}


/*
=================
Mod_ReadAliasModel

The model in the file (mod_end its end), built in the scratch; false, with
mod_error, if it can't be used
=================
*/
static bool Mod_ReadAliasModel (model_t *mod, const void *buffer, aliashdr_t **header)
{
	int					i;
	mdl_t				*pmodel;
	const mdl_t			*pinmodel = buffer;
	stvert_t			*pstverts;
	const stvert_t		*pinstverts;
	aliashdr_t			*pheader;
	mtriangle_t			*ptri;
	const dtriangle_t	*pintriangles;
	int					version, numframes, numskins, nverts, ntris, skinwidth, skinheight;
	size_t				size, filesize;
	const daliasframetype_t	*pframetype;
	const daliasskintype_t	*pskintype;
	maliasskindesc_t	*pskindesc;
	int					skinsize;

	if (!Mod_InFile (pinmodel, sizeof(*pinmodel), 1))
		return false;
	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		return Mod_FileError ("it has version %i, not %i", version, ALIAS_VERSION);

	// the counts, which the file must hold, before any space is made for them
	filesize = (size_t)(mod_end - (const byte *)buffer);
	numskins = LittleLong (pinmodel->numskins);
	skinwidth = LittleLong (pinmodel->skinwidth);
	skinheight = LittleLong (pinmodel->skinheight);
	nverts = LittleLong (pinmodel->numverts);
	ntris = LittleLong (pinmodel->numtris);
	numframes = LittleLong (pinmodel->numframes);
	if (skinwidth <= 0 || skinheight <= 0 || (size_t)skinwidth * (size_t)skinheight > filesize)
		return Mod_FileError ("its skins are %i by %i", skinwidth, skinheight);
	if (nverts <= 0)
		return Mod_FileError ("it has no vertices");
	if (nverts > MAXALIASVERTS)
		return Mod_FileError ("it has %i vertices, more than %i", nverts, MAXALIASVERTS);
	if (ntris <= 0 || (size_t)ntris > filesize / sizeof(dtriangle_t))
		return Mod_FileError ("it has %i triangles", ntris);
	if (numskins < 1 || (size_t)numskins > filesize / sizeof(daliasskintype_t))
		return Mod_FileError ("it has %i skins", numskins);
	if (numframes < 1 || (size_t)numframes > filesize / sizeof(daliasframetype_t))
		return Mod_FileError ("it has %i frames", numframes);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size = 	sizeof (aliashdr_t) + (size_t)(numframes - 1) * sizeof (pheader->frames[0]) +
			sizeof (mdl_t) +
			(size_t)nverts * sizeof (stvert_t) +
			(size_t)ntris * sizeof (mtriangle_t);

	pheader = Mod_ScratchAlloc (size);
	*header = pheader;
	pmodel = (mdl_t *) ((byte *)&pheader[1] +
			(numframes - 1) * sizeof (pheader->frames[0]));

	mod->flags = LittleLong (pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
	pmodel->boundingradius = LittleFloat (pinmodel->boundingradius);
	pmodel->numskins = numskins;
	pmodel->skinwidth = skinwidth;
	pmodel->skinheight = skinheight;
	pmodel->numverts = nverts;
	pmodel->numtris = ntris;
	pmodel->numframes = numframes;
	pmodel->size = (float)(LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO);
	mod->synctype = LittleLong (pinmodel->synctype);
	mod->numframes = pmodel->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pmodel->scale[i] = LittleFloat (pinmodel->scale[i]);
		pmodel->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pmodel->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}

	// any skin width: id's needed four for its assembly (QuakeSpasm has dropped
	// the check too; Copper's null models are 2 by 1)
	pheader->model = (int)((byte *)pmodel - (byte *)pheader);

//
// load the skins
//
	skinsize = pmodel->skinheight * pmodel->skinwidth;

	pskintype = (const daliasskintype_t *)&pinmodel[1];

	pskindesc = Mod_ScratchAlloc (numskins * sizeof (maliasskindesc_t));

	pheader->skindesc = (int)((byte *)pskindesc - (byte *)pheader);

	for (i=0 ; i<numskins ; i++)
	{
		aliasskintype_t	skintype;

		if (!Mod_InFile (pskintype, sizeof(*pskintype), 1))
			return false;
		skintype = LittleLong (pskintype->type);
		pskindesc[i].type = skintype;

		if (skintype == ALIAS_SKIN_SINGLE)
		{
			pskintype = Mod_LoadAliasSkin (pskintype + 1,
										   &pskindesc[i].skin,
										   skinsize, pheader);
		}
		else
		{
			pskintype = Mod_LoadAliasSkinGroup (pskintype + 1,
												&pskindesc[i].skin,
												skinsize, pheader);
		}
		if (!pskintype)
			return false;
	}

//
// set base s and t vertices
//
	pstverts = (stvert_t *)&pmodel[1];
	pinstverts = (const stvert_t *)pskintype;
	if (!Mod_InFile (pinstverts, sizeof(*pinstverts), (size_t)nverts))
		return false;

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
	pintriangles = (const dtriangle_t *)&pinstverts[pmodel->numverts];
	if (!Mod_InFile (pintriangles, sizeof(*pintriangles), (size_t)ntris))
		return false;

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
	pframetype = (const daliasframetype_t *)&pintriangles[pmodel->numtris];

	for (i=0 ; i<numframes ; i++)
	{
		aliasframetype_t	frametype;

		if (!Mod_InFile (pframetype, sizeof(*pframetype), 1))
			return false;
		frametype = LittleLong (pframetype->type);
		pheader->frames[i].type = frametype;


		if (frametype == ALIAS_SINGLE)
		{
			pframetype = Mod_LoadAliasFrame (pframetype + 1,
											&pheader->frames[i].frame,
											pmodel->numverts,
											&pheader->frames[i].bboxmin,
											&pheader->frames[i].bboxmax,
											pheader, pheader->frames[i].name);
		}
		else
		{
			pframetype = Mod_LoadAliasGroup (pframetype + 1,
											&pheader->frames[i].frame,
											pmodel->numverts,
											&pheader->frames[i].bboxmin,
											&pheader->frames[i].bboxmax,
											pheader, pheader->frames[i].name);
		}
		if (!pframetype)
			return false;
	}
	return true;
}

/*
=================
Mod_LoadAliasModel

False, with mod_error, if the file can't be used
=================
*/
static bool Mod_LoadAliasModel (model_t *mod, const void *buffer, int filesize)
{
	aliashdr_t	*pheader = NULL;
	size_t		start, total;
	bool		ok;

	start = mod_scratch_used;
	mod_end = (const byte *)buffer + filesize;
	ok = Mod_ReadAliasModel (mod, buffer, &pheader);
	if (ok)
	{
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
	}
	mod_scratch_used = start;
	return ok;
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
static const void *Mod_LoadSpriteFrame (const void *pin, mspriteframe_t **ppframe)
{
	const dspriteframe_t	*pinframe = pin;
	mspriteframe_t			*pspriteframe;
	int						width, height, size, origin[2];

	if (!Mod_InFile (pinframe, sizeof(*pinframe), 1))
		return NULL;
	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	if (width <= 0 || height <= 0 || !Mod_InFile (pinframe + 1, (size_t)width, (size_t)height))
	{
		Mod_FileError ("a frame is %i by %i", width, height);
		return NULL;
	}
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

	Q_memcpy (&pspriteframe->pixels[0], (const byte *)(pinframe + 1), size);

	return (const byte *)pinframe + sizeof (dspriteframe_t) + size;
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
static const void *Mod_LoadSpriteGroup (const void *pin, mspriteframe_t **ppframe)
{
	const dspritegroup_t	*pingroup = pin;
	mspritegroup_t			*pspritegroup;
	int						i, numframes;
	const dspriteinterval_t	*pin_intervals;
	float					*poutintervals;
	const void				*ptemp;

	if (!Mod_InFile (pingroup, sizeof(*pingroup), 1))
		return NULL;
	numframes = LittleLong (pingroup->numframes);
	pin_intervals = (const dspriteinterval_t *)(pingroup + 1);
	if (numframes < 1 || !Mod_InFile (pin_intervals, sizeof(*pin_intervals), (size_t)numframes))
	{
		Mod_FileError ("a frame group has %i frames", numframes);
		return NULL;
	}

	pspritegroup = Mod_Alloc (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]));

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	poutintervals = Mod_Alloc (numframes * sizeof (float));

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
		{
			Mod_FileError ("a frame group's interval is %g", *poutintervals);
			return NULL;
		}

		poutintervals++;
		pin_intervals++;
	}

	ptemp = pin_intervals;

	for (i=0 ; i<numframes && ptemp ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i]);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel

False, with mod_error, if the file can't be used
=================
*/
static bool Mod_LoadSpriteModel (model_t *mod, const void *buffer, int filesize)
{
	int					i;
	int					version;
	const dsprite_t		*pin = buffer;
	msprite_t			*psprite;
	int					numframes;
	size_t				size;
	const dspriteframetype_t	*pframetype;

	mod_end = (const byte *)buffer + filesize;
	if (!Mod_InFile (pin, sizeof(*pin), 1))
		return false;
	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		return Mod_FileError ("it has version %i, not %i", version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);
	if (numframes < 1 || (size_t)numframes > (size_t)filesize / sizeof(dspriteframetype_t))
		return Mod_FileError ("it has %i frames", numframes);

	size = sizeof (msprite_t) +	(size_t)(numframes - 1) * sizeof (psprite->frames);

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
	mod->numframes = numframes;

	pframetype = (const dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t	frametype;

		if (!Mod_InFile (pframetype, sizeof(*pframetype), 1))
			return false;
		frametype = LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = Mod_LoadSpriteFrame (pframetype + 1,
											 &psprite->frames[i].frameptr);
		}
		else
		{
			pframetype = Mod_LoadSpriteGroup (pframetype + 1,
											 &psprite->frames[i].frameptr);
		}
		if (!pframetype)
			return false;
	}

	mod->type = mod_sprite;
	return true;
}

//=============================================================================

