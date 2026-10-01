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
// d_edge.c

#include "r_local.h"
#include "d_local.h"

static int	miplevel;

float		scale_for_mip;
extern int			screenwidth;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;

static vec3_t		transformed_modelorg;

/*
=============
D_MipLevelForScale
=============
*/
static int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface
==============
*/

static void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	pixel_t	*pdest, pix;
	int		u;

	pix = d_pal30[color & 255];
	for (span=surf->spans ; span ; span=span->pnext)
	{
		pdest = d_viewbuffer + screenwidth*span->v + span->u;
		for (u = 0 ; u < span->count ; u++)
			pdest[u] = pix;
	}
}


/*
==============
D_FaceTexmap

A face's texture mapping at a mip level into map, seen with the view's right,
up and forward vectors in its model's space from transformed_org (the view's
origin in the face's model, transformed); map's 1/z is left as it is. Any
thread may.
==============
*/
static void D_FaceTexmap (simd_texmap_t *map, const msurface_t *pface, int mip, const vec3_t transformed_org,
	const vec3_t right, const vec3_t up, const vec3_t forward)
{
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	mipscale = 1.0f / (float)(1 << mip);

	p_saxis[0] = DotProduct (pface->texinfo->vecs[0], right);
	p_saxis[1] = DotProduct (pface->texinfo->vecs[0], up);
	p_saxis[2] = DotProduct (pface->texinfo->vecs[0], forward);
	p_taxis[0] = DotProduct (pface->texinfo->vecs[1], right);
	p_taxis[1] = DotProduct (pface->texinfo->vecs[1], up);
	p_taxis[2] = DotProduct (pface->texinfo->vecs[1], forward);

	t = xscaleinv * mipscale;
	map->sdivzstepu = p_saxis[0] * t;
	map->tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	map->sdivzstepv = -p_saxis[1] * t;
	map->tdivzstepv = -p_taxis[1] * t;

	map->sdivzorigin = p_saxis[2] * mipscale - xcenter * map->sdivzstepu -
			ycenter * map->sdivzstepv;
	map->tdivzorigin = p_taxis[2] * mipscale - xcenter * map->tdivzstepu -
			ycenter * map->tdivzstepv;

	p_temp1[0] = transformed_org[0] * mipscale;
	p_temp1[1] = transformed_org[1] * mipscale;
	p_temp1[2] = transformed_org[2] * mipscale;

	t = 0x10000*mipscale;
	// the terms are 16.16 texture coordinates and can pass 32 bits (texture
	// coordinates beyond +-32768); their sum is relative to the surface
	map->sadjust = (fixed16_t)(((int64_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) -
			(((int64_t)pface->texturemins[0] * 0x10000) >> mip)
			+ pface->texinfo->vecs[0][3]*t);
	map->tadjust = (fixed16_t)(((int64_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) -
			(((int64_t)pface->texturemins[1] * 0x10000) >> mip)
			+ pface->texinfo->vecs[1][3]*t);

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	map->sextent = ((pface->extents[0] << 16) >> mip) - 1;
	map->textent = ((pface->extents[1] << 16) >> mip) - 1;
}


/*
==============
D_CalcGradients

D_FaceTexmap in the drawing globals, for miplevel and the view as it is
==============
*/
static void D_CalcGradients (msurface_t *pface)
{
	simd_texmap_t	map;

	D_FaceTexmap (&map, pface, miplevel, transformed_modelorg, vright, vup, vpn);
	d_sdivzstepu = map.sdivzstepu;
	d_tdivzstepu = map.tdivzstepu;
	d_sdivzstepv = map.sdivzstepv;
	d_tdivzstepv = map.tdivzstepv;
	d_sdivzorigin = map.sdivzorigin;
	d_tdivzorigin = map.tdivzorigin;
	sadjust = map.sadjust;
	tadjust = map.tadjust;
	bbextents = map.sextent;
	bbextentt = map.textent;
}


/*
==============
D_DrawFence
==============
*/
void D_DrawFence (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump, float nearzi)
{
	surfcache_t	*cache;

	VectorCopy (transformed_org, transformed_modelorg);
	miplevel = D_MipLevelForScale (nearzi * scale_for_mip * surf->texinfo->mipadjust);
	miplevel = D_SurfaceMipLevel (surf, miplevel);
	cache = D_CacheSurface (surf, miplevel);
	cacheblock = cache->data;
	cachewidth = cache->width;
	D_CalcGradients (surf);
	D_DrawFencePolygon (pverts, nump);
}

/*
==============
D_DrawTranslucentFace

A surface blended into the frame: a liquid from its texture, as Turbulent8
draws it, anything else from its cache block, fence holes left out
==============
*/
void D_DrawTranslucentFace (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump,
	float nearzi, int alpha)
{
	surfcache_t	*cache;

	VectorCopy (transformed_org, transformed_modelorg);
	if (surf->flags & SURF_DRAWTURB)
	{
		miplevel = 0;
		d_turbsource = (byte *)surf->texinfo->texture + surf->texinfo->texture->offsets[0];
		d_turbsource30 = R_TextureOverride (surf->texinfo->texture, 0);
		D_CalcGradients (surf);
		D_DrawBlendedPolygon (pverts, nump, alpha, true);
		return;
	}
	miplevel = D_MipLevelForScale (nearzi * scale_for_mip * surf->texinfo->mipadjust);
	miplevel = D_SurfaceMipLevel (surf, miplevel);
	cache = D_CacheSurface (surf, miplevel);
	cacheblock = cache->data;
	cachewidth = cache->width;
	D_CalcGradients (surf);
	D_DrawBlendedPolygon (pverts, nump, alpha, false);
}


/*
===============================================================================

THE SURFACES OF A FRAME

The surfaces whose spans the bands' scans have gathered are prepared as jobs
spread over the worker threads (how to draw, mip level, texture mapping, in
a brush model's turned view), then given their cache blocks on this thread,
and drawn in batches: the cache blocks that must be drawn first, and then the
surfaces' spans, both spread over the workers. Each pixel is in the spans of
one surface only, so the order the surfaces are drawn in makes no difference.

===============================================================================
*/

typedef enum
{
	DS_SOLID,			// one color
	DS_SKY,
	DS_SKYBOX,
	DS_TURB,			// a liquid, from its texture
	DS_CACHED			// from its surface cache block
} dsdraw_t;

typedef struct
{
	surf_t			*surf;
	dsdraw_t		draw;
	int				color;			// DS_SOLID: palette index
	simd_texmap_t	map;			// the texture mapping, and the 1/z of all
	const byte		*turb;			// DS_TURB: the 64x64 texture,
	const pixel_t	*turb30;		// or a TGA file's texels in its place
	msurface_t		*face;			// DS_CACHED: the surface, its entity,
	entity_t		*entity;
	int				miplevel;		// mip level and block
	surfcache_t		*cache;
	bool			build;			// the block is drawn first, as buildsurf says
	drawsurf_t		buildsurf;
	int				texels;			// drawn into the block
} dsjob_t;

static dsjob_t	*d_jobs;			// a surface with spans each
static surf_t	**d_drawn;			// those surfaces
static int		*d_builds;			// the jobs of a batch whose blocks are drawn first
static int		d_numjobs, d_maxjobs;
static vec3_t	world_transformed_modelorg;

#define D_PREPARE_JOBS	128			// prepared by a thread at a time

/*
==============
D_PrepareJob

A surface with spans as a job, but for its cache block: on any thread, so
the view isn't turned for a brush model's surface, but its vectors worked out
==============
*/
static void D_PrepareJob (dsjob_t *job, surf_t *s)
{
	msurface_t	*pface = s->data;
	vec3_t		local, transformed_org, right, up, forward;
	int			mip;

	*job = (dsjob_t){.surf = s, .entity = s->entity};
	job->map.ziorigin = s->d_ziorigin;
	job->map.zistepu = s->d_zistepu;
	job->map.zistepv = s->d_zistepv;

	if (s->flags & SURF_DRAWSKY)
	{
		job->draw = r_skyfaces ? DS_SKYBOX : DS_SKY;
		return;
	}
	if (s->flags & SURF_DRAWBACKGROUND)
	{
	// the background is infinitely far: 1/z is 0
		job->map.zistepu = 0;
		job->map.zistepv = 0;
		job->map.ziorigin = 0;
		job->draw = DS_SOLID;
		job->color = (int)r_clearcolor.value & 0xFF;
		return;
	}

	if (s->insubmodel)
	{
		VectorSubtract (r_origin, s->entity->origin, local);
		TransformVector (local, transformed_org);
		R_EntityViewVectors (s->entity, right, up, forward);
	}
	else
	{
		VectorCopy (world_transformed_modelorg, transformed_org);
		VectorCopy (vright, right);
		VectorCopy (vup, up);
		VectorCopy (vpn, forward);
	}

	if (s->flags & SURF_DRAWTURB)
	{
		mip = 0;
		job->draw = DS_TURB;
		job->turb = (byte *)pface->texinfo->texture + pface->texinfo->texture->offsets[0];
		job->turb30 = R_TextureOverride (pface->texinfo->texture, 0);
	}
	else
	{
		mip = D_MipLevelForScale (s->nearzi * scale_for_mip * pface->texinfo->mipadjust);
		mip = D_SurfaceMipLevel (pface, mip);
		job->draw = DS_CACHED;
		job->face = pface;
		job->miplevel = mip;
	}
	D_FaceTexmap (&job->map, pface, mip, transformed_org, right, up, forward);
}

// D_PREPARE_JOBS of the jobs prepared
static void D_PrepareJobs (void *ctx, int index)
{
	int		i, end;

	(void)ctx;
	end = (index + 1) * D_PREPARE_JOBS < d_numjobs ? (index + 1) * D_PREPARE_JOBS : d_numjobs;
	for (i = index * D_PREPARE_JOBS ; i<end ; i++)
		D_PrepareJob (&d_jobs[i], d_drawn[i]);
}

// a job's block drawn (D_PrepareCacheSurface)
static void D_BuildJob (void *ctx, int index)
{
	dsjob_t	*job = &d_jobs[((int *)ctx)[index]];

	job->texels = D_DrawCacheSurface (&job->buildsurf);
}

// a job's spans and their 1/z
static void D_DrawJob (void *ctx, int index)
{
	dsjob_t	*job = &((dsjob_t *)ctx)[index];
	espan_t	*spans = job->surf->spans;

	switch (job->draw)
	{
	case DS_SOLID:
		D_DrawSolidSurface (job->surf, job->color);
		break;
	case DS_SKY:
		D_DrawSkyScans (spans);
		break;
	case DS_SKYBOX:
		D_DrawSkyboxScans (spans);
		break;
	case DS_TURB:
		Turbulent8 (spans, &job->map, job->turb, job->turb30);
		break;
	case DS_CACHED:
		D_DrawSpans (spans, &job->map, job->cache->data, (int)job->cache->width);
		break;
	}
	D_DrawZSpans (spans, &job->map);
}

/*
==============
D_DrawBatch

The jobs from first to end, a batch: their blocks, then their spans
==============
*/
static void D_DrawBatch (int first, int end)
{
	double	prof;
	int		i, numbuilds, texels;

	numbuilds = 0;
	for (i=first ; i<end ; i++)
		if (d_jobs[i].build)
			d_builds[numbuilds++] = i;
	prof = R_ProfStart ();
	Sys_Parallel (numbuilds, D_BuildJob, d_builds);
	R_ProfEnd (PROF_SURFCACHE, prof);
	Sys_Parallel (end - first, D_DrawJob, d_jobs + first);

	texels = 0;
	for (i=first ; i<end ; i++)
		texels += d_jobs[i].texels;
	R_ProfCount (PROFN_TEXELS, texels);
	R_ProfCount (PROFN_BATCHES, 1);
	D_BeginSurfaceBatch ();
}


/*
==============
D_DrawSurfaces

The surfaces' spans the bands' scans have gathered
==============
*/
void D_DrawSurfaces (rband_t *bands, int numbands)
{
	surf_t			*s;
	rband_t			*b;
	dsjob_t			*job;
	cacheprep_t		prep;
	int				i, first;
	double			prof = R_ProfStart ();

	currententity = &r_worldentity;
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);

// TODO: could preset a lot of this at mode set time
	if (r_drawflat.value)
	{
		for (b = bands ; b<bands + numbands ; b++)
		{
			for (s = &b->surfaces[1] ; s<b->surface_p ; s++)
			{
				simd_texmap_t	map = {.ziorigin = s->d_ziorigin, .zistepu = s->d_zistepu, .zistepv = s->d_zistepv};

				if (!s->spans)
					continue;

				D_DrawSolidSurface (s, (int)((intptr_t)s->data & 0xFF));
				D_DrawZSpans (s->spans, &map);
			}
		}
		R_ProfEnd (PROF_DRAW, prof);
		return;
	}

// the surfaces with spans, and their jobs prepared
	d_numjobs = 0;
	for (b = bands ; b<bands + numbands ; b++)
	{
		for (s = &b->surfaces[1] ; s<b->surface_p ; s++)
		{
			if (!s->spans)
				continue;

			if (d_numjobs == d_maxjobs)
			{
				d_maxjobs = d_maxjobs ? d_maxjobs * 2 : 256;
				d_jobs = Mem_Realloc (d_jobs, (size_t)d_maxjobs * sizeof(*d_jobs));
				d_drawn = Mem_Realloc (d_drawn, (size_t)d_maxjobs * sizeof(*d_drawn));
				d_builds = Mem_Realloc (d_builds, (size_t)d_maxjobs * sizeof(*d_builds));
			}
			if ((s->flags & SURF_DRAWSKY) && !r_skyfaces && !r_skymade)
				R_MakeSky ();
			d_drawn[d_numjobs++] = s;
		}
	}
	r_drawnpolycount += d_numjobs;
	Sys_Parallel ((d_numjobs + D_PREPARE_JOBS - 1) / D_PREPARE_JOBS, D_PrepareJobs, NULL);

// their cache blocks, in batches of jobs no two of which have the same block
// or the room for one; a batch is drawn when a job's block is taken
	D_BeginSurfaceBatch ();
	first = 0;
	for (i=0 ; i<d_numjobs ; i++)
	{
		job = &d_jobs[i];
		if (job->draw != DS_CACHED)
			continue;
		currententity = job->entity;		// its texture's animation
		while ((prep = D_PrepareCacheSurface (job->face, job->miplevel, &job->cache, &job->buildsurf)) == CACHE_TAKEN)
		{
			D_DrawBatch (first, i);
			first = i;
		}
		job->build = prep == CACHE_DRAW;
	}
	currententity = &r_worldentity;
	D_DrawBatch (first, d_numjobs);
	R_ProfEnd (PROF_DRAW, prof);
}
