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
R_AddAfter

A fence or translucent surface a band met, of its entity: R_MergeAfters adds
them to the lists when the bands have run
================
*/
void R_AddAfter (rband_t *b, msurface_t *surf, int alpha)
{
	rafter_t	*after;

	if (b->numafters == b->maxafters)
	{
		b->outofafters = true;
		return;
	}
	after = &b->afters[b->numafters++];
	after->surf = surf;
	after->entity = b->entity;
	after->alpha = alpha;
}

/*
================
R_MergeAfters

The fences and translucent surfaces the bands met, each once
================
*/
void R_MergeAfters (void)
{
	rband_t		*b;
	rafter_t	*after;

	R_ClearFences ();
	for (b = r_bands ; b<r_bands + r_numbands ; b++)
	{
		for (after = b->afters ; after<b->afters + b->numafters ; after++)
		{
			currententity = after->entity;
			if (after->alpha < 256)
				R_AddTranslucent (after->surf, after->alpha);
			else
				R_AddFence (after->surf);
		}
	}
	currententity = &r_worldentity;
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
R_ProjectFence

A fence or translucent surface clipped to the view and projected into
points (room for its edges and 5 more), from the view in its model's space,
with its nearest 1/z and its 1/z gradients; verts is room for clipping, twice
as many. False if none of it is in the view. Any thread's.
================
*/
typedef struct
{
	vec3_t		org;					// modelorg
	vec3_t		right, up, forward;		// vright, vup, vpn
	clipplane_t	planes[4];				// view_clipplanes
} fenceview_t;

typedef struct
{
	int			nump;
	float		nearzi;
	float		ziorigin, zistepu, zistepv;
} fenceproj_t;

// as TransformVector, by the view's vectors
static void R_FenceTransform (const fenceview_t *view, const vec3_t in, vec3_t out)
{
	out[0] = DotProduct (in, view->right);
	out[1] = DotProduct (in, view->up);
	out[2] = DotProduct (in, view->forward);
}

// the view as the globals have it for the current model
static void R_CurrentFenceView (fenceview_t *view)
{
	VectorCopy (modelorg, view->org);
	VectorCopy (vright, view->right);
	VectorCopy (vup, view->up);
	VectorCopy (vpn, view->forward);
	memcpy (view->planes, view_clipplanes, sizeof(view->planes));
}

static bool R_ProjectFence (msurface_t *surf, model_t *model, const fenceview_t *view, emitpoint_t *points,
	vec3_t *verts[2], fenceproj_t *out)
{
	int			i, e, nump, cur;
	float		distinv, nearzi, scale, area;
	vec3_t		local, transformed, p_normal;
	mvertex_t	*v;
	mplane_t	*plane;
	emitpoint_t	*pout, swap;

	nump = surf->numedges;
	for (i = 0 ; i < nump ; i++)
	{
		e = model->surfedges[surf->firstedge + i];
		v = &model->vertexes[e >= 0 ? model->edges[e].v[0] : model->edges[-e].v[1]];
		VectorCopy (v->position, verts[0][i]);
	}

	// to the view, in the model's space
	cur = 0;
	for (i = 0 ; i < 4 ; i++)
	{
		nump = R_ClipFence (verts[cur], nump, verts[!cur], &view->planes[i]);
		cur = !cur;
		if (nump < 3)
			return false;
	}

	nearzi = 0;
	area = 0;
	for (i = 0 ; i < nump ; i++)
	{
		VectorSubtract (verts[cur][i], view->org, local);
		R_FenceTransform (view, local, transformed);
		if (transformed[2] < NEAR_CLIP)
			transformed[2] = (vec_t)NEAR_CLIP;

		pout = &points[i];
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
		area += points[i].u * points[e].v - points[e].u * points[i].v;
	}
	if (area < 0)
	{
		for (i = 0 ; i < nump / 2 ; i++)
		{
			swap = points[i];
			points[i] = points[nump - 1 - i];
			points[nump - 1 - i] = swap;
		}
	}

	// 1/z on the screen, from the face's plane, as R_RenderFace works it out
	plane = surf->plane;
	distinv = 1.0f / (plane->dist - DotProduct (view->org, plane->normal));
	R_FenceTransform (view, plane->normal, p_normal);
	out->nump = nump;
	out->nearzi = nearzi;
	out->zistepu = p_normal[0] * xscaleinv * distinv;
	out->zistepv = -p_normal[1] * yscaleinv * distinv;
	out->ziorigin = p_normal[2] * distinv - xcenter * out->zistepu - ycenter * out->zistepv;
	return true;
}

/*
================
R_DrawProjectedFence

A projected fence or translucent surface handed to the drawer
================
*/
static void R_DrawProjectedFence (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *points,
	const fenceproj_t *proj, int alpha)
{
	d_zistepu = proj->zistepu;
	d_zistepv = proj->zistepv;
	d_ziorigin = proj->ziorigin;
	if (alpha < 256)
		D_DrawTranslucentFace (surf, transformed_org, points, proj->nump, proj->nearzi, alpha);
	else
		D_DrawFence (surf, transformed_org, points, proj->nump, proj->nearzi);
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
	fenceproj_t	proj;
	fenceview_t	view;

	if (surf->numedges < 3)
		return;
	if (surf->numedges + 5 > r_maxfenceverts)
	{
		r_maxfenceverts = surf->numedges + 5;
		r_fenceverts[0] = Mem_Realloc (r_fenceverts[0], (size_t)r_maxfenceverts * sizeof(vec3_t));
		r_fenceverts[1] = Mem_Realloc (r_fenceverts[1], (size_t)r_maxfenceverts * sizeof(vec3_t));
		r_fencepoints = Mem_Realloc (r_fencepoints, (size_t)r_maxfenceverts * sizeof(emitpoint_t));
	}
	R_CurrentFenceView (&view);
	if (R_ProjectFence (surf, model, &view, r_fencepoints, r_fenceverts, &proj))
		R_DrawProjectedFence (surf, transformed_org, r_fencepoints, &proj, alpha);
}

/*
================
R_PrepareFences

The projections of a pass's count surfaces (surfs[i] of entities[i]; NULL
for none) worked out on the worker threads before they are drawn in their
order: each surface's is its own, from the view in its model's space as
R_DrawSurfaceAfter sets it up, so the same.
================
*/
typedef struct
{
	msurface_t	*surf;
	entity_t	*entity;
	int			first;		// its points in r_preppoints
	bool		ok;
	fenceproj_t	proj;
} fenceprep_t;

static fenceprep_t	*r_preps;
static int			r_maxpreps;
static emitpoint_t	*r_preppoints;
static int			r_maxpreppoints;
static fenceview_t	r_worldfenceview;	// the world's, as R_DrawSurfaceAfter has it

static void R_PrepareFence (void *ctx, int index)
{
	static thread_local vec3_t	*verts[2];
	static thread_local int		maxverts;
	fenceprep_t					*prep = &r_preps[index];
	fenceview_t					entview;
	const fenceview_t			*view = &r_worldfenceview;
	int							need = prep->surf->numedges + 5;

	(void)ctx;
	if (need > maxverts)
	{
		maxverts = need;
		verts[0] = Mem_Realloc (verts[0], (size_t)maxverts * sizeof(vec3_t));
		verts[1] = Mem_Realloc (verts[1], (size_t)maxverts * sizeof(vec3_t));
	}
	if (prep->entity != &r_worldentity)
	{
		R_EntityModelView (prep->entity, entview.org, entview.right, entview.up, entview.forward);
		R_ViewFrustum (entview.right, entview.up, entview.forward, entview.org, entview.planes);
		view = &entview;
	}
	prep->ok = R_ProjectFence (prep->surf, prep->entity == &r_worldentity ? r_scene.worldmodel : prep->entity->model,
		view, r_preppoints + prep->first, verts, &prep->proj);
}

// the lists' surfaces and entities as R_PrepareFences takes them, and the
// index of each one's prep, -1 for none
static msurface_t	**r_prepsurfs;
static entity_t		**r_prepentities;
static int			*r_prepindex;
static int			r_maxprepitems;

static void R_PrepareItems (int count)
{
	if (count <= r_maxprepitems)
		return;
	r_maxprepitems = count;
	r_prepsurfs = Mem_Realloc (r_prepsurfs, (size_t)count * sizeof(*r_prepsurfs));
	r_prepentities = Mem_Realloc (r_prepentities, (size_t)count * sizeof(*r_prepentities));
	r_prepindex = Mem_Realloc (r_prepindex, (size_t)count * sizeof(*r_prepindex));
}

static void R_PrepareFences (int count)
{
	int		i, n, points;

	if (count > r_maxpreps)
	{
		r_maxpreps = count;
		r_preps = Mem_Realloc (r_preps, (size_t)r_maxpreps * sizeof(*r_preps));
	}
	for (i = 0, n = 0, points = 0 ; i < count ; i++)
	{
		r_prepindex[i] = -1;
		if (!r_prepsurfs[i] || r_prepsurfs[i]->numedges < 3)
			continue;
		r_prepindex[i] = n;
		r_preps[n].surf = r_prepsurfs[i];
		r_preps[n].entity = r_prepentities[i];
		r_preps[n].first = points;
		points += r_prepsurfs[i]->numedges + 5;
		n++;
	}
	if (points > r_maxpreppoints)
	{
		r_maxpreppoints = points;
		r_preppoints = Mem_Realloc (r_preppoints, (size_t)r_maxpreppoints * sizeof(*r_preppoints));
	}

	// the world's view: modelorg its origin, the vectors and planes the
	// world's (as each entity's surface leaves them)
	R_CurrentFenceView (&r_worldfenceview);
	VectorCopy (r_origin, r_worldfenceview.org);
	Sys_Parallel (n, R_PrepareFence, NULL);
}

/*
================
R_DrawSurfaceAfter

A fence or translucent surface of an entity: in its model's space, as
D_DrawSurfaces sets it up, then back in the world's. Projected already if
prep isn't NULL.
================
*/
static void R_DrawSurfaceAfter (msurface_t *surf, entity_t *entity, int alpha, const fenceprep_t *prep)
{
	vec3_t	transformed_org;

	currententity = entity;
	if (entity == &r_worldentity)
	{
		VectorCopy (r_origin, modelorg);
		TransformVector (modelorg, transformed_org);
		if (!prep)
			R_DrawFence (surf, r_scene.worldmodel, transformed_org, alpha);
		else if (prep->ok)
			R_DrawProjectedFence (surf, transformed_org, r_preppoints + prep->first, &prep->proj, alpha);
		return;
	}

	VectorSubtract (r_origin, entity->origin, modelorg);
	TransformVector (modelorg, transformed_org);
	R_RotateBmodel ();
	if (!prep)
		R_DrawFence (surf, entity->model, transformed_org, alpha);
	else if (prep->ok)
		R_DrawProjectedFence (surf, transformed_org, r_preppoints + prep->first, &prep->proj, alpha);

	VectorCopy (base_vpn, vpn);
	VectorCopy (base_vup, vup);
	VectorCopy (base_vright, vright);
	VectorCopy (base_modelorg, modelorg);
	R_TransformFrustum ();
}

// i's prep, or NULL
static const fenceprep_t *R_Prep (int i)
{
	return r_prepindex[i] >= 0 ? &r_preps[r_prepindex[i]] : NULL;
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

	R_PrepareItems (r_numfences);
	for (i = 0 ; i < r_numfences ; i++)
	{
		r_prepsurfs[i] = r_fences[i].surf;
		r_prepentities[i] = r_fences[i].entity;
	}
	R_PrepareFences (r_numfences);
	for (i = 0 ; i < r_numfences ; i++)
		R_DrawSurfaceAfter (r_fences[i].surf, r_fences[i].entity, 256, R_Prep (i));
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
			alpha = R_SurfaceAlpha (currententity, psurf);
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
// the list the farthest first, those as far in the order they were added: a
// radix sort of the distances' bits, two passes of 11 and one of 10, each
// keeping the order of equal digits. A distance is a sum of squares, never
// negative, so its bits order as it does, and inverted they put the
// farthest first.
static void R_SortTranslucent (void)
{
	static translucent_t	*scratch;
	static int				maxscratch;
	static const int		shifts[3] = {0, 11, 22};
	translucent_t			*from = r_translucent, *to, *swap;
	uint32_t				key;
	int						count[2048], pass, i, sum, c, bits;

	if (r_numtranslucent > maxscratch)
	{
		maxscratch = r_maxtranslucent;
		scratch = Mem_Realloc (scratch, (size_t)maxscratch * sizeof(*scratch));
	}
	to = scratch;
	for (pass = 0 ; pass < 3 ; pass++)
	{
		bits = pass < 2 ? 11 : 10;
		memset (count, 0, sizeof(count));
		for (i = 0 ; i < r_numtranslucent ; i++)
		{
			memcpy (&key, &from[i].dist, sizeof(key));
			count[(~key >> shifts[pass]) & ((1u << bits) - 1)]++;
		}
		for (i = 0, sum = 0 ; i < (1 << bits) ; i++)
		{
			c = count[i];
			count[i] = sum;
			sum += c;
		}
		for (i = 0 ; i < r_numtranslucent ; i++)
		{
			memcpy (&key, &from[i].dist, sizeof(key));
			to[count[(~key >> shifts[pass]) & ((1u << bits) - 1)]++] = from[i];
		}
		swap = from;
		from = to;
		to = swap;
	}
	if (from != r_translucent)		// an odd number of passes ends in the scratch
		memcpy (r_translucent, from, (size_t)r_numtranslucent * sizeof(*r_translucent));
}

void R_DrawTranslucent (void)
{
	translucent_t	*t;
	int				i;

	if (!r_numtranslucent)
		return;
	R_SortTranslucent ();
	R_PrepareItems (r_numtranslucent);
	for (i = 0, t = r_translucent ; i < r_numtranslucent ; i++, t++)
	{
		r_prepsurfs[i] = t->surf;
		r_prepentities[i] = t->entity;
	}
	R_PrepareFences (r_numtranslucent);
	for (i = 0, t = r_translucent ; i < r_numtranslucent ; i++, t++)
	{
		if (t->surf)
		{
			R_DrawSurfaceAfter (t->surf, t->entity, t->alpha, R_Prep (i));
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
