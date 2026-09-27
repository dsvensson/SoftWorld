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
// d_surf.c: rasterization driver surface heap manager

#include "r_local.h"
#include "d_local.h"
#include "r_local.h"

bool        r_cache_thrash;         // set if surface cache is thrashing

// Surfaces are prepared together and then drawn together (D_DrawSurfaces), in
// batches; the cache blocks of a batch are marked with it, and stay theirs
// until the batch is drawn
static unsigned	d_batch;
static bool		sc_resumewrapped;	// the allocation the batch stopped had wrapped the rover

static int                                     sc_size;
surfcache_t *sc_rover;
static surfcache_t *sc_base;

#define GUARDSIZE       4


int     D_SurfaceCacheForRes (int width, int height)
{
	int             size, pix;

	if (COM_CheckParm ("-surfcachesize"))
	{
		size = Q_atoi(com_argv[COM_CheckParm("-surfcachesize")+1]) * 1024;
		return size;
	}
	
	size = SURFCACHE_SIZE_AT_320X200;

	pix = width*height;
	if (pix > 64000)
		size += (pix-64000)*3;

	return size * (int)sizeof(pixel_t);
}

static void D_CheckCacheGuard (void)
{
	byte    *s;
	int             i;

	s = (byte *)sc_base + sc_size;
	for (i=0 ; i<GUARDSIZE ; i++)
		if (s[i] != (byte)i)
			Sys_Error ("D_CheckCacheGuard: failed");
}

static void D_ClearCacheGuard (void)
{
	byte    *s;
	int             i;
	
	s = (byte *)sc_base + sc_size;
	for (i=0 ; i<GUARDSIZE ; i++)
		s[i] = (byte)i;
}


/*
================
D_InitCaches

================
*/
void D_InitCaches (void *buffer, int size)
{
//		Con_Printf ("%ik surface cache\n", size/1024);

	sc_size = size - GUARDSIZE;
	sc_base = (surfcache_t *)buffer;
	sc_rover = sc_base;
	
	sc_base->next = NULL;
	sc_base->owner = NULL;
	sc_base->size = sc_size;
	
	D_ClearCacheGuard ();
}


/*
==================
D_FlushCaches
==================
*/
void D_FlushCaches (void)
{
	surfcache_t     *c;
	
	if (!sc_base)
		return;

	for (c = sc_base ; c ; c = c->next)
	{
		if (c->owner)
			*c->owner = NULL;
	}
	
	sc_rover = sc_base;
	sc_base->next = NULL;
	sc_base->owner = NULL;
	sc_base->size = sc_size;
}

/*
=================
D_SurfaceMipLevel

A mip level at least miplevel whose block takes at most a quarter of the
cache, for surfaces of the largest faces
=================
*/
int D_SurfaceMipLevel (msurface_t *surface, int miplevel)
{
	while (miplevel < MIPLEVELS - 1
		&& (int64_t)(surface->extents[0] >> miplevel) * (surface->extents[1] >> miplevel) * (int64_t)sizeof(pixel_t) > sc_size / 4)
		miplevel++;
	return miplevel;
}

// takes a block from its surface; false if the current batch has it
static bool D_SCFree (surfcache_t *block)
{
	if (!block->owner)
		return true;
	if (block->batch == d_batch)
		return false;
	*block->owner = NULL;
	return true;
}

/*
=================
D_SCAlloc

A block for a surface, made of the blocks after the rover, those cached
longest ago. NULL if it would take a block of the current batch: then the
batch is drawn, and the allocation made again in the next, where it goes on
from where it stopped, so blocks are taken as if surfaces were drawn one by
one.
=================
*/
static surfcache_t     *D_SCAlloc (int width, int size)
{
	surfcache_t             *new;
	bool                wrapped_this_time;

	if ((width < 0) || (width > 4096))
		Sys_Error ("D_SCAlloc: bad cache width %d\n", width);

	if (size <= 0)
		Sys_Error ("D_SCAlloc: bad cache size %d\n", size);
	
	size = (int)offsetof (surfcache_t, data) + size;
	size = (size + 3) & ~3;
	if (size > sc_size)
		Sys_Error ("D_SCAlloc: %i > cache size",size);

// if there is not size bytes after the rover, reset to the start
	wrapped_this_time = sc_resumewrapped;
	sc_resumewrapped = false;

	if ( !sc_rover || (byte *)sc_rover - (byte *)sc_base > sc_size - size)
	{
		if (sc_rover)
		{
			wrapped_this_time = true;
		}
		sc_rover = sc_base;
	}
		
// colect and free surfcache_t blocks until the rover block is large enough
	new = sc_rover;
	if (!D_SCFree (new))
	{
		sc_resumewrapped = wrapped_this_time;
		return NULL;
	}

	while (new->size < size)
	{
	// free another
		if (!new->next)
			Sys_Error ("D_SCAlloc: hit the end of memory");
		if (!D_SCFree (new->next))
		{	// what is freed so far is one free block, the next try's start
			new->owner = NULL;
			sc_rover = new;
			sc_resumewrapped = wrapped_this_time;
			return NULL;
		}
		sc_rover = new->next;

		new->size += sc_rover->size;
		new->next = sc_rover->next;
	}

// create a fragment out of any leftovers
	if (new->size - size > 256)
	{
		sc_rover = (surfcache_t *)( (byte *)new + size);
		sc_rover->size = new->size - size;
		sc_rover->next = new->next;
		sc_rover->width = 0;
		sc_rover->owner = NULL;
		new->next = sc_rover;
		new->size = size;
	}
	else
		sc_rover = new->next;
	
	new->width = width;
// DEBUG
	if (width > 0)
		new->height = (size - sizeof(*new) + sizeof(new->data)) / width;

	new->owner = NULL;              // should be set properly after return

	if (d_roverwrapped)
	{
		if (wrapped_this_time || (sc_rover >= d_initial_rover))
			r_cache_thrash = true;
	}
	else if (wrapped_this_time)
	{       
		d_roverwrapped = true;
	}

D_CheckCacheGuard ();   // DEBUG
	return new;
}

//=============================================================================

void D_BeginSurfaceBatch (void)
{
	d_batch++;
}

/*
================
D_PrepareCacheSurface

The surface's cache block at the mip level, allocated if it has none, for
the current batch, in *pcache. CACHE_READY: its texels are as they must be.
CACHE_DRAW: they must be drawn first, as *draw says (D_DrawCacheSurface).
CACHE_TAKEN: the block, or the room for it, is an earlier surface's of the
batch, so the batch must be drawn before this surface is prepared again, in
a new batch.
================
*/
cacheprep_t D_PrepareCacheSurface (msurface_t *surface, int miplevel, surfcache_t **pcache, drawsurf_t *draw)
{
	texture_t		*texture;
	fixed8_t		lightadj[MAXLIGHTMAPS];
	surfcache_t		*cache;
	int				lightcount, width, height, i;

//
// if the surface is animating or flashing, flush the cache
//
	texture = R_TextureAnimation (surface->texinfo->texture);
	for (i=0 ; i<MAXLIGHTMAPS ; i++)
		lightadj[i] = d_lightstylevalue[surface->styles[i]];

//
// see if the cache holds apropriate data
//
	cache = surface->cachespots[miplevel];
	*pcache = cache;

	if (cache && !cache->dlight && surface->dlightframe != r_framecount
			&& cache->texture == texture
			&& cache->lightadj[0] == lightadj[0]
			&& cache->lightadj[1] == lightadj[1]
			&& cache->lightadj[2] == lightadj[2]
			&& cache->lightadj[3] == lightadj[3] )
	{
		cache->batch = d_batch;
		return CACHE_READY;
	}
	if (cache && cache->batch == d_batch)
		return CACHE_TAKEN;

//
// determine shape of surface
//
	width = surface->extents[0] >> miplevel;
	height = surface->extents[1] >> miplevel;

//
// allocate memory if needed, with room for the light the texels are drawn with
//
	lightcount = R_SurfaceLightCount (surface, miplevel);
	if (!cache)     // if a texture just animated, don't reallocate it
	{
		cache = D_SCAlloc (width, width * height * (int)sizeof(pixel_t) + lightcount * (int)sizeof(unsigned));
		if (!cache)
			return CACHE_TAKEN;
		surface->cachespots[miplevel] = cache;
		cache->owner = &surface->cachespots[miplevel];
		cache->mipscale = 1.0f / (1<<miplevel);
		cache->lightcount = 0;
		*pcache = cache;
	}
	cache->batch = d_batch;

	if (surface->dlightframe == r_framecount)
		cache->dlight = 1;
	else
		cache->dlight = 0;

	*draw = (drawsurf_t){
		.surfdat = cache->data,
		.rowpixels = width,
		.surf = surface,
		.texture = texture,
		.surfmip = miplevel,
		.surfwidth = width,
		.surfheight = height,
		// texels drawn with the same texture are drawn again only where the light changed
		.keptlight = lightcount ? (unsigned *)(cache->data + width * height) : NULL,
		.keptvalid = cache->lightcount == lightcount && cache->texture == texture,
	};
	for (i=0 ; i<MAXLIGHTMAPS ; i++)
		draw->lightadj[i] = lightadj[i];
	cache->lightcount = lightcount;

	cache->texture = texture;
	cache->lightadj[0] = lightadj[0];
	cache->lightadj[1] = lightadj[1];
	cache->lightadj[2] = lightadj[2];
	cache->lightadj[3] = lightadj[3];

	c_surf++;
	R_ProfCount (PROFN_SURFACES, 1);
	R_ProfCount (PROFN_DLIT, cache->dlight);
	return CACHE_DRAW;
}

/*
================
D_DrawCacheSurface

Draws and lights a prepared cache block; returns the texels drawn. Any
thread may draw blocks, each a block of its own.
================
*/
int D_DrawCacheSurface (const drawsurf_t *draw)
{
	r_drawsurf = *draw;
	return R_DrawSurface ();
}

/*
================
D_CacheSurface

The surface's cache block at the mip level, drawn now if it must be
================
*/
surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel)
{
	double			prof;
	surfcache_t		*cache;
	drawsurf_t		draw;

	D_BeginSurfaceBatch ();		// a batch of one: its block is drawn at once
	if (D_PrepareCacheSurface (surface, miplevel, &cache, &draw) == CACHE_DRAW)
	{
		prof = R_ProfStart ();
		R_ProfCount (PROFN_TEXELS, D_DrawCacheSurface (&draw));
		R_ProfEnd (PROF_SURFCACHE, prof);
	}
	return cache;
}
