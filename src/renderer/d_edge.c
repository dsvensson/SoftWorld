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
D_CalcGradients
==============
*/
static void D_CalcGradients (msurface_t *pface)
{
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	mipscale = 1.0f / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	// the terms are 16.16 texture coordinates and can pass 32 bits (texture
	// coordinates beyond +-32768); their sum is relative to the surface
	sadjust = (fixed16_t)(((int64_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) -
			(((int64_t)pface->texturemins[0] * 0x10000) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t);
	tadjust = (fixed16_t)(((int64_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) -
			(((int64_t)pface->texturemins[1] * 0x10000) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t);

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
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

Drawn in batches: the surfaces whose spans the edge scan has gathered are
prepared one by one on this thread (mip level, texture mapping, cache block,
and a rotated brush model's view), then the cache blocks that must be drawn
are drawn, and then the surfaces' spans, both spread over the worker threads.
Each pixel is in the spans of one surface only, so the order the surfaces are
drawn in makes no difference.

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

static dsjob_t	*d_jobs;			// the batch
static int		*d_builds;			// the jobs whose blocks are drawn first
static int		d_numjobs, d_maxjobs;
static vec3_t	world_transformed_modelorg;

/*
==============
D_PrepareSurface

A surface with spans as a job of the batch. Returns false if an earlier job
of the batch has the cache block it needs, or the room for it: the batch is
drawn first, and the surface prepared again in the next.
==============
*/
static bool D_PrepareSurface (surf_t *s, dsjob_t *job)
{
	msurface_t	*pface = s->data;
	vec3_t		local_modelorg;
	cacheprep_t	prep = CACHE_READY;

	*job = (dsjob_t){.surf = s};
	d_zistepu = s->d_zistepu;
	d_zistepv = s->d_zistepv;
	d_ziorigin = s->d_ziorigin;

	if (s->flags & SURF_DRAWSKY)
	{
		if (r_skyfaces)
			job->draw = DS_SKYBOX;
		else
		{
			if (!r_skymade)
				R_MakeSky ();
			job->draw = DS_SKY;
		}
	}
	else if (s->flags & SURF_DRAWBACKGROUND)
	{
	// the background is infinitely far: 1/z is 0
		d_zistepu = 0;
		d_zistepv = 0;
		d_ziorigin = 0;
		job->draw = DS_SOLID;
		job->color = (int)r_clearcolor.value & 0xFF;
	}
	else
	{
		if (s->insubmodel)
		{
		// FIXME: we don't want to do all this for every polygon!
		// TODO: store once at start of frame
			currententity = s->entity;	//FIXME: make this passed in to
										// R_RotateBmodel ()
			VectorSubtract (r_origin, currententity->origin, local_modelorg);
			TransformVector (local_modelorg, transformed_modelorg);

			R_RotateBmodel ();	// FIXME: don't mess with the frustum,
								// make entity passed in
		}

		if (s->flags & SURF_DRAWTURB)
		{
			miplevel = 0;
			job->draw = DS_TURB;
			job->turb = (byte *)pface->texinfo->texture + pface->texinfo->texture->offsets[0];
			job->turb30 = R_TextureOverride (pface->texinfo->texture, 0);
		}
		else
		{
			miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip * pface->texinfo->mipadjust);
			miplevel = D_SurfaceMipLevel (pface, miplevel);
			prep = D_PrepareCacheSurface (pface, miplevel, &job->cache, &job->buildsurf);
			job->draw = DS_CACHED;
			job->face = pface;
			job->entity = currententity;
			job->miplevel = miplevel;
			job->build = prep == CACHE_DRAW;
		}
		D_CalcGradients (pface);

		if (s->insubmodel)
		{
		//
		// restore the old drawing state
		// FIXME: we don't want to do this every time!
		// TODO: speed up
		//
			VectorCopy (world_transformed_modelorg, transformed_modelorg);
			VectorCopy (base_vpn, vpn);
			VectorCopy (base_vup, vup);
			VectorCopy (base_vright, vright);
			VectorCopy (base_modelorg, modelorg);
			R_TransformFrustum ();
			currententity = &r_worldentity;
		}
	}

	job->map = D_SpanTexmap ();
	return prep != CACHE_TAKEN;
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

The prepared surfaces: their blocks, then their spans
==============
*/
static void D_DrawBatch (void)
{
	double	prof;
	int		i, numbuilds, texels;

	numbuilds = 0;
	for (i=0 ; i<d_numjobs ; i++)
		if (d_jobs[i].build)
			d_builds[numbuilds++] = i;
	prof = R_ProfStart ();
	Sys_Parallel (numbuilds, D_BuildJob, d_builds);
	R_ProfEnd (PROF_SURFCACHE, prof);
	Sys_Parallel (d_numjobs, D_DrawJob, d_jobs);

	texels = 0;
	for (i=0 ; i<d_numjobs ; i++)
		texels += d_jobs[i].texels;
	R_ProfCount (PROFN_TEXELS, texels);
	R_ProfCount (PROFN_BATCHES, 1);
	d_numjobs = 0;
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

	D_BeginSurfaceBatch ();
	for (b = bands ; b<bands + numbands ; b++)
	{
		for (s = &b->surfaces[1] ; s<b->surface_p ; s++)
		{
			if (!s->spans)
				continue;

			r_drawnpolycount++;
			if (d_numjobs == d_maxjobs)
			{
				d_maxjobs = d_maxjobs ? d_maxjobs * 2 : 256;
				d_jobs = Mem_Realloc (d_jobs, (size_t)d_maxjobs * sizeof(*d_jobs));
				d_builds = Mem_Realloc (d_builds, (size_t)d_maxjobs * sizeof(*d_builds));
			}
			while (!D_PrepareSurface (s, &d_jobs[d_numjobs]))
				D_DrawBatch ();
			d_numjobs++;
		}
	}
	D_DrawBatch ();
	R_ProfEnd (PROF_DRAW, prof);
}
