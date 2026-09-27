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
// r_fence.c -- surfaces the edge sort can't draw, because it gives every
// pixel to one surface and what's behind them would never be drawn:
// - fences ({ texture names), whose texture index 255 is cut out: drawn
//   after the world, depth tested and depth writing, skipping the holes;
// - translucent surfaces (liquids with r_*alpha, brush models with an
//   alpha): blended in after the models, back to front with translucent
//   alias models, depth tested but not depth writing.

#include "r_local.h"
#include "d_local.h"

typedef struct
{
	msurface_t	*surf;
	entity_t	*entity;
} fence_t;

static fence_t	*r_fences;
static int		r_numfences, r_maxfences;
static int		r_fencepass;		// the edge pass the lists belong to

typedef struct
{
	msurface_t	*surf;				// NULL for an alias model
	entity_t	*entity;
	int			alpha;				// of 256
	float		dist;				// from the view, squared; the far ones are drawn first
} translucent_t;

static translucent_t	*r_translucent;
static int		r_numtranslucent, r_maxtranslucent;

static vec3_t	*r_fenceverts[2];	// clipping ping-pong
static emitpoint_t	*r_fencepoints;
static int		r_maxfenceverts;

/*
================
R_ClearFences

At the start of every edge pass (a pass is redone when its edges overflow)
================
*/
void R_ClearFences (void)
{
	r_numfences = 0;
	r_numtranslucent = 0;
	r_fencepass++;
}

/*
================
R_AddFence

A fence surface of currententity, instead of its edges; the pieces a clipped
brush model face comes in are added once
================
*/
void R_AddFence (msurface_t *surf)
{
	if (surf->fencepass == r_fencepass && surf->fenceentity == currententity)
		return;
	surf->fencepass = r_fencepass;
	surf->fenceentity = currententity;

	if (r_numfences == r_maxfences)
	{
		r_maxfences = r_maxfences ? r_maxfences * 2 : 64;
		r_fences = Mem_Realloc (r_fences, (size_t)r_maxfences * sizeof(*r_fences));
	}
	r_fences[r_numfences].surf = surf;
	r_fences[r_numfences].entity = currententity;
	r_numfences++;
}

/*
================
R_ClipFence

The part of a polygon on the front of a clipping plane
================
*/
static int R_ClipFence (vec3_t *in, int nump, vec3_t *out, const clipplane_t *plane)
{
	float	dists[2], frac;
	int		i, j, outcount = 0;

	for (i = 0 ; i < nump ; i++)
	{
		j = i + 1 < nump ? i + 1 : 0;
		dists[0] = DotProduct (in[i], plane->normal) - plane->dist;
		dists[1] = DotProduct (in[j], plane->normal) - plane->dist;
		if (dists[0] >= 0)
		{
			VectorCopy (in[i], out[outcount]);		// a macro: no ++ in it
			outcount++;
		}
		if (dists[0] == 0 || dists[1] == 0 || (dists[0] > 0) == (dists[1] > 0))
			continue;
		frac = dists[0] / (dists[0] - dists[1]);
		out[outcount][0] = in[i][0] + frac * (in[j][0] - in[i][0]);
		out[outcount][1] = in[i][1] + frac * (in[j][1] - in[i][1]);
		out[outcount][2] = in[i][2] + frac * (in[j][2] - in[i][2]);
		outcount++;
	}
	return outcount;
}

/*
================
R_DrawFence

A fence surface in the current model's space (modelorg, the view vectors and
the clip planes set for it): clipped to the view, projected, and handed to
the drawer with its 1/z gradients
================
*/
static void R_DrawFence (msurface_t *surf, model_t *model, const vec3_t transformed_org, int alpha)
{
	int			i, e, nump, cur;
	float		distinv, nearzi, scale, area;
	vec3_t		local, transformed, p_normal;
	mvertex_t	*v;
	mplane_t	*plane;
	emitpoint_t	*pout, swap;

	nump = surf->numedges;
	if (nump < 3)
		return;
	if (nump + 5 > r_maxfenceverts)
	{
		r_maxfenceverts = nump + 5;
		r_fenceverts[0] = Mem_Realloc (r_fenceverts[0], (size_t)r_maxfenceverts * sizeof(vec3_t));
		r_fenceverts[1] = Mem_Realloc (r_fenceverts[1], (size_t)r_maxfenceverts * sizeof(vec3_t));
		r_fencepoints = Mem_Realloc (r_fencepoints, (size_t)r_maxfenceverts * sizeof(emitpoint_t));
	}

	for (i = 0 ; i < nump ; i++)
	{
		e = model->surfedges[surf->firstedge + i];
		v = &model->vertexes[e >= 0 ? model->edges[e].v[0] : model->edges[-e].v[1]];
		VectorCopy (v->position, r_fenceverts[0][i]);
	}

	// to the view, in the model's space
	cur = 0;
	for (i = 0 ; i < 4 ; i++)
	{
		nump = R_ClipFence (r_fenceverts[cur], nump, r_fenceverts[!cur], &view_clipplanes[i]);
		cur = !cur;
		if (nump < 3)
			return;
	}

	nearzi = 0;
	area = 0;
	for (i = 0 ; i < nump ; i++)
	{
		VectorSubtract (r_fenceverts[cur][i], modelorg, local);
		TransformVector (local, transformed);
		if (transformed[2] < NEAR_CLIP)
			transformed[2] = (vec_t)NEAR_CLIP;

		pout = &r_fencepoints[i];
		pout->zi = 1.0f / transformed[2];
		if (pout->zi > nearzi)
			nearzi = pout->zi;
		scale = xscale * pout->zi;
		pout->u = xcenter + scale * transformed[0];
		scale = yscale * pout->zi;
		pout->v = ycenter - scale * transformed[1];
		pout->s = pout->t = 0;
	}

	// the scanner wants the winding a front face has on screen
	for (i = 0 ; i < nump ; i++)
	{
		e = i + 1 < nump ? i + 1 : 0;
		area += r_fencepoints[i].u * r_fencepoints[e].v - r_fencepoints[e].u * r_fencepoints[i].v;
	}
	if (area < 0)
	{
		for (i = 0 ; i < nump / 2 ; i++)
		{
			swap = r_fencepoints[i];
			r_fencepoints[i] = r_fencepoints[nump - 1 - i];
			r_fencepoints[nump - 1 - i] = swap;
		}
	}

	// 1/z on the screen, from the face's plane, as R_RenderFace works it out
	plane = surf->plane;
	distinv = 1.0f / (plane->dist - DotProduct (modelorg, plane->normal));
	TransformVector (plane->normal, p_normal);
	d_zistepu = p_normal[0] * xscaleinv * distinv;
	d_zistepv = -p_normal[1] * yscaleinv * distinv;
	d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu - ycenter * d_zistepv;

	if (alpha < 256)
		D_DrawTranslucentFace (surf, transformed_org, r_fencepoints, nump, nearzi, alpha);
	else
		D_DrawFence (surf, transformed_org, r_fencepoints, nump, nearzi);
}

/*
================
R_DrawSurfaceAfter

A fence or translucent surface of an entity: in its model's space, as
D_DrawSurfaces sets it up, then back in the world's
================
*/
static void R_DrawSurfaceAfter (msurface_t *surf, entity_t *entity, int alpha)
{
	vec3_t	transformed_org;

	currententity = entity;
	if (entity == &r_worldentity)
	{
		VectorCopy (r_origin, modelorg);
		TransformVector (modelorg, transformed_org);
		R_DrawFence (surf, r_scene.worldmodel, transformed_org, alpha);
		return;
	}

	VectorSubtract (r_origin, entity->origin, modelorg);
	TransformVector (modelorg, transformed_org);
	R_RotateBmodel ();
	R_DrawFence (surf, entity->model, transformed_org, alpha);

	VectorCopy (base_vpn, vpn);
	VectorCopy (base_vup, vup);
	VectorCopy (base_vright, vright);
	VectorCopy (base_modelorg, modelorg);
	R_TransformFrustum ();
}

/*
================
R_DrawFences

After the world's surfaces, before the models: every fence surface of the pass
================
*/
void R_DrawFences (void)
{
	int		i;

	for (i = 0 ; i < r_numfences ; i++)
		R_DrawSurfaceAfter (r_fences[i].surf, r_fences[i].entity, 256);
	currententity = &r_worldentity;
}

/*
================
R_NewTranslucent
================
*/
static translucent_t *R_NewTranslucent (entity_t *entity, int alpha, const vec3_t center)
{
	translucent_t	*t;
	vec3_t			d;

	if (r_numtranslucent == r_maxtranslucent)
	{
		r_maxtranslucent = r_maxtranslucent ? r_maxtranslucent * 2 : 64;
		r_translucent = Mem_Realloc (r_translucent, (size_t)r_maxtranslucent * sizeof(*r_translucent));
	}
	t = &r_translucent[r_numtranslucent++];
	t->surf = NULL;
	t->entity = entity;
	t->alpha = alpha;
	VectorSubtract (center, r_origin, d);
	t->dist = DotProduct (d, d);
	return t;
}

/*
================
R_AddTranslucent

A translucent surface of currententity, instead of its edges; sorted by the
distance to its middle. The pieces a clipped brush model face comes in are
added once.
================
*/
void R_AddTranslucent (msurface_t *surf, int alpha)
{
	model_t		*model = currententity->model;
	mvertex_t	*v;
	vec3_t		center;
	int			i, e;

	if (surf->fencepass == r_fencepass && surf->fenceentity == currententity)
		return;
	surf->fencepass = r_fencepass;
	surf->fenceentity = currententity;
	if (surf->numedges < 3)
		return;

	center[0] = center[1] = center[2] = 0;
	for (i = 0 ; i < surf->numedges ; i++)
	{
		e = model->surfedges[surf->firstedge + i];
		v = &model->vertexes[e >= 0 ? model->edges[e].v[0] : model->edges[-e].v[1]];
		VectorAdd (center, v->position, center);
	}
	VectorScale (center, 1.0f / surf->numedges, center);
	VectorAdd (center, currententity->origin, center);
	R_NewTranslucent (currententity, alpha, center)->surf = surf;
}

/*
================
R_AddTranslucentModel

The faces of currententity, a translucent brush model, that face the view;
modelorg is in the model's space
================
*/
void R_AddTranslucentModel (model_t *model)
{
	msurface_t	*psurf;
	float		dot;
	int			i, alpha;

	psurf = &model->surfaces[model->firstmodelsurface];
	for (i = 0 ; i < model->nummodelsurfaces ; i++, psurf++)
	{
		dot = DotProduct (modelorg, psurf->plane->normal) - psurf->plane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			alpha = R_SurfaceAlpha (psurf);
			if (alpha < 256)
				R_AddTranslucent (psurf, alpha);
		}
	}
}

/*
================
R_AddTranslucentEntity

A translucent alias model
================
*/
void R_AddTranslucentEntity (entity_t *ent)
{
	R_NewTranslucent (ent, R_EntityAlpha (ent), ent->origin);
}

/*
================
R_DrawTranslucent

After the models: the translucent surfaces and models, the farthest first
================
*/
static int R_TranslucentOrder (const void *a, const void *b)
{
	float	da = ((const translucent_t *)a)->dist, db = ((const translucent_t *)b)->dist;

	return da < db ? 1 : da > db ? -1 : 0;
}

void R_DrawTranslucent (void)
{
	translucent_t	*t;
	int				i;

	if (!r_numtranslucent)
		return;
	qsort (r_translucent, (size_t)r_numtranslucent, sizeof(*r_translucent), R_TranslucentOrder);
	for (i = 0, t = r_translucent ; i < r_numtranslucent ; i++, t++)
	{
		if (t->surf)
		{
			R_DrawSurfaceAfter (t->surf, t->entity, t->alpha);
			continue;
		}
		currententity = t->entity;
		d_alpha = t->alpha;
		R_DrawAliasEntity ();
		d_alpha = 256;
	}
	currententity = &r_worldentity;
	VectorCopy (r_origin, modelorg);
}
