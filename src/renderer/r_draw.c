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

// r_draw.c

#include "r_local.h"
#include "r_local.h"
#include "d_local.h"	// FIXME: shouldn't need to include this

// a band's edgecache of a world edge no line has (horizontal or clipped away),
// with the band's pass: not the frame's count, as a band that ran out of room
// runs again in the same frame
#define FULLY_CLIPPED_CACHED	0x80000000
#define PASS_MASK				0x7FFFFFFF

int			c_faceclip;					// number of faces clipped

clipplane_t	view_clipplanes[4];

int		sintable[CYCLE*2];
int		*intsintable;

/*
================
R_EmitEdge
================
*/
static void R_EmitEdge (rband_t *b, mvertex_t *pv0, mvertex_t *pv1)
{
	edge_t	*edge;
	float	u, u_step;
	vec3_t	local, transformed;
	float	*world;
	int		v, v2, ceilv0;
	float	scale, lzi0, u0, v0;
	int		side;

	if (b->lastvertvalid)
	{
		u0 = b->u1;
		v0 = b->v1;
		lzi0 = b->lzi1;
		ceilv0 = b->ceilv1;
	}
	else
	{
		world = &pv0->position[0];
	
	// transform and project
		VectorSubtract (world, b->modelorg, local);
		R_BandTransform (b, local, transformed);
	
		if (transformed[2] < NEAR_CLIP)
			transformed[2] = (vec_t)NEAR_CLIP;
	
		lzi0 = 1.0f / transformed[2];
	
	// FIXME: build x/yscale into transform?
		scale = xscale * lzi0;
		u0 = (xcenter + scale*transformed[0]);
		if (u0 < r_refdef.fvrectx_adj)
			u0 = r_refdef.fvrectx_adj;
		if (u0 > r_refdef.fvrectright_adj)
			u0 = r_refdef.fvrectright_adj;
	
		scale = yscale * lzi0;
		v0 = (ycenter - scale*transformed[1]);
		if (v0 < r_refdef.fvrecty_adj)
			v0 = r_refdef.fvrecty_adj;
		if (v0 > r_refdef.fvrectbottom_adj)
			v0 = r_refdef.fvrectbottom_adj;
	
		ceilv0 = (int) ceil(v0);
	}

	world = &pv1->position[0];

// transform and project
	VectorSubtract (world, b->modelorg, local);
	R_BandTransform (b, local, transformed);

	if (transformed[2] < NEAR_CLIP)
		transformed[2] = (vec_t)NEAR_CLIP;

	b->lzi1 = 1.0f / transformed[2];

	scale = xscale * b->lzi1;
	b->u1 = (xcenter + scale*transformed[0]);
	if (b->u1 < r_refdef.fvrectx_adj)
		b->u1 = r_refdef.fvrectx_adj;
	if (b->u1 > r_refdef.fvrectright_adj)
		b->u1 = r_refdef.fvrectright_adj;

	scale = yscale * b->lzi1;
	b->v1 = (ycenter - scale*transformed[1]);
	if (b->v1 < r_refdef.fvrecty_adj)
		b->v1 = r_refdef.fvrecty_adj;
	if (b->v1 > r_refdef.fvrectbottom_adj)
		b->v1 = r_refdef.fvrectbottom_adj;

	if (b->lzi1 > lzi0)
		lzi0 = b->lzi1;

	if (lzi0 > b->nearzi)	// for mipmap finding
		b->nearzi = lzi0;

// for right edges, all we want is the effect on 1/z
	if (b->nearzionly)
		return;

	b->emitted = true;

	b->ceilv1 = (int) ceil(b->v1);


// create the edge
	if (ceilv0 == b->ceilv1)
	{
	// we cache unclipped horizontal edges as fully clipped, with their 1/z
	// for the other face that has them to count too
		if (b->cacheoffset != 0x7FFFFFFF)
		{
			b->cacheoffset = FULLY_CLIPPED_CACHED |
					(b->pass & PASS_MASK);
			b->cachenearzi = lzi0;
		}

		return;		// horizontal edge
	}

	side = ceilv0 > b->ceilv1;

	edge = b->edge_p++;

	edge->owner = b->pedge;

	edge->nearzi = lzi0;

	if (side == 0)
	{
	// trailing edge (go from p1 to p2)
		v = ceilv0;
		v2 = b->ceilv1 - 1;

		edge->surfs[0] = (uint32_t)(b->surface_p - b->surfaces);
		edge->surfs[1] = 0;

		u_step = ((b->u1 - u0) / (b->v1 - v0));
		u = u0 + ((float)v - v0) * u_step;
	}
	else
	{
	// leading edge (go from p2 to p1)
		v2 = ceilv0 - 1;
		v = b->ceilv1;

		edge->surfs[0] = 0;
		edge->surfs[1] = (uint32_t)(b->surface_p - b->surfaces);

		u_step = ((u0 - b->u1) / (v0 - b->v1));
		u = b->u1 + ((float)v - b->v1) * u_step;
	}

	edge->u_step = (int64_t)(u_step*0x100000);
	edge->u = (int64_t)(u*0x100000 + 0xFFFFF);

// we need to do this to avoid stepping off the edges if a very nearly
// horizontal edge is less than epsilon above a scan, and numeric error causes
// it to incorrectly extend to the scan, and the extension of the line goes off
// the edge of the screen
// FIXME: is this actually needed?
	if (edge->u < r_refdef.vrect_x_adj_shift20)
		edge->u = r_refdef.vrect_x_adj_shift20;
	if (edge->u > r_refdef.vrectright_adj_shift20)
		edge->u = r_refdef.vrectright_adj_shift20;

// sorted when the scan starts (R_SortNewEdges), by the line it starts on even
// if that's above the band; a leading edge may be given a trailing surface
// later, so the kind is kept apart
	b->edgestarts[edge - b->edges] = (uint32_t)v << 1 | (side == 0);

// the band's lines of it, from the first as the scan would have stepped it
// there; one with none is kept for the edge caching and the surfaces' 1/z
	if (v < b->top)
	{
		edge->u += (int64_t)(b->top - v) * edge->u_step;
		v = b->top;
	}
	if (v2 >= b->bottom)
		v2 = b->bottom - 1;
	if (v > v2)
	{
		b->edgestarts[edge - b->edges] = EDGE_OUTSIDE;
		return;
	}

	edge->nextremove = b->removeedges[v2];
	b->removeedges[v2] = edge;
}


/*
================
R_ClipEdge
================
*/
static void R_ClipEdge (rband_t *b, mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip)
{
	float		d0, d1, f;
	mvertex_t	clipvert;

	if (clip)
	{
		do
		{
			d0 = DotProduct (pv0->position, clip->normal) - clip->dist;
			d1 = DotProduct (pv1->position, clip->normal) - clip->dist;

			if (d0 >= 0)
			{
			// point 0 is unclipped
				if (d1 >= 0)
				{
				// both points are unclipped
					continue;
				}

			// only point 1 is clipped

			// we don't cache clipped edges
				b->cacheoffset = 0x7FFFFFFF;

				f = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					b->leftclipped = true;
					b->leftexit = clipvert;
				}
				else if (clip->rightedge)
				{
					b->rightclipped = true;
					b->rightexit = clipvert;
				}

				R_ClipEdge (b, pv0, &clipvert, clip->next);
				return;
			}
			else
			{
			// point 0 is clipped
				if (d1 < 0)
				{
				// both points are clipped
				// we do cache fully clipped edges, but not what's left of one
				// a plane has clipped already: the other face would miss what
				// that clip did for the face
					if (b->cacheoffset != 0x7FFFFFFF)
					{
						b->cacheoffset = FULLY_CLIPPED_CACHED |
								(b->pass & PASS_MASK);
						b->cachenearzi = 0;
					}
					return;
				}

			// only point 0 is clipped
				b->lastvertvalid = false;

			// we don't cache partially clipped edges
				b->cacheoffset = 0x7FFFFFFF;

				f = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					b->leftclipped = true;
					b->leftenter = clipvert;
				}
				else if (clip->rightedge)
				{
					b->rightclipped = true;
					b->rightenter = clipvert;
				}

				R_ClipEdge (b, &clipvert, pv1, clip->next);
				return;
			}
		} while ((clip = clip->next) != NULL);
	}

// add the edge
	R_EmitEdge (b, pv0, pv1);
}



/*
================
R_EmitCachedEdge

The band's edge at offset, made for another face, given this one too
================
*/
static void R_EmitCachedEdge (rband_t *b, unsigned offset)
{
	edge_t		*pedge_t;

	pedge_t = (edge_t *)((uintptr_t)b->edges + offset);

	if (!pedge_t->surfs[0])
		pedge_t->surfs[0] = (uint32_t)(b->surface_p - b->surfaces);
	else
		pedge_t->surfs[1] = (uint32_t)(b->surface_p - b->surfaces);

	if (pedge_t->nearzi > b->nearzi)	// for mipmap finding
		b->nearzi = pedge_t->nearzi;

	b->emitted = true;
}


/*
================
R_RenderFace
================
*/
void R_RenderFace (rband_t *b, msurface_t *fa, int clipflags)
{
	int			i, lindex, alpha;
	unsigned	mask, cached;
	bool		reversed;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	medge_t		*pedges;
	clipplane_t	*pclip;

	if (fa->flags & SURF_NOTEXELS)
		return;

// a translucent surface is blended in after the models; a fence mustn't
// hide what's behind its holes: drawn after the world
	alpha = R_SurfaceAlpha (b->entity, fa);
	if (alpha < 256 || (fa->flags & SURF_DRAWFENCE))
	{
		R_AddAfter (b, fa, alpha);
		return;
	}

// skip out if no more surfs
	if ((b->surface_p) >= b->surf_max)
	{
		b->outofsurfaces = true;
		return;
	}

// ditto if not enough edges left
	if ((b->edge_p + fa->numedges + 4) >= b->edge_max)
	{
		b->outofedges = true;
		return;
	}

	b->faceclip++;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
			b->clipplanes[i].next = pclip;
			pclip = &b->clipplanes[i];
		}
	}

// push the edges through
	b->emitted = false;
	b->nearzi = 0;
	b->nearzionly = false;
	b->makeleftedge = b->makerightedge = false;
	pedges = b->entity->model->edges;
	b->lastvertvalid = false;

	for (i=0 ; i<fa->numedges ; i++)
	{
		lindex = b->entity->model->surfedges[fa->firstedge + i];
		reversed = lindex <= 0;
		if (reversed)
			lindex = -lindex;
		b->pedge = &pedges[lindex];

	// if the edge is cached, we can just reuse the edge: a world edge found to
	// have no lines, or one the band made for the other face it is shared with
	// (Mod_MarkSharedEdges), owned by this medge_t
		if (!b->insubmodel)
		{
			cached = b->edgecache[lindex];
			if (cached & FULLY_CLIPPED_CACHED)
			{
				if ((cached & PASS_MASK) == (b->pass & PASS_MASK))
				{
					if (b->edgenearzi[lindex] > b->nearzi)	// for mipmap finding
						b->nearzi = b->edgenearzi[lindex];
					b->lastvertvalid = false;
					continue;
				}
			}
			else if (b->pedge->shared && (((uintptr_t)b->edge_p - (uintptr_t)b->edges) > cached) &&
				(((edge_t *)((uintptr_t)b->edges + cached))->owner == b->pedge))
			{
				R_EmitCachedEdge (b, cached);
				b->lastvertvalid = false;
				continue;
			}
		}

	// assume it's cacheable
		b->cacheoffset = (unsigned int)((byte *)b->edge_p - (byte *)b->edges);
		b->leftclipped = b->rightclipped = false;
		if (reversed)
			R_ClipEdge (b, &b->vertbase[b->pedge->v[1]], &b->vertbase[b->pedge->v[0]], pclip);
		else
			R_ClipEdge (b, &b->vertbase[b->pedge->v[0]], &b->vertbase[b->pedge->v[1]], pclip);
		if (!b->insubmodel)
		{
			b->edgecache[lindex] = b->cacheoffset;
			if (b->cacheoffset & FULLY_CLIPPED_CACHED)
				b->edgenearzi[lindex] = b->cachenearzi;
		}

		if (b->leftclipped)
			b->makeleftedge = true;
		if (b->rightclipped)
			b->makerightedge = true;
		b->lastvertvalid = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (b->makeleftedge)
	{
		b->pedge = &b->tedge;
		b->lastvertvalid = false;
		R_ClipEdge (b, &b->leftexit, &b->leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right nearzi
	if (b->makerightedge)
	{
		b->pedge = &b->tedge;
		b->lastvertvalid = false;
		b->nearzionly = true;
		R_ClipEdge (b, &b->rightexit, &b->rightenter, b->clipplanes[1].next);
	}

// if no edges made it out, return without posting the surface
	if (!b->emitted)
		return;

	b->polycount++;

	b->surface_p->data = (void *)fa;
	b->surface_p->nearzi = b->nearzi;
	b->surface_p->flags = fa->flags;
	b->surface_p->insubmodel = b->insubmodel;
	b->surface_p->spanstate = 0;
	b->surface_p->entity = b->entity;
	b->surface_p->key = b->currentkey++;
	b->surface_p->spans = NULL;

	pplane = fa->plane;
// FIXME: cache this?
	R_BandTransform (b, pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0f / (pplane->dist - DotProduct (b->modelorg, pplane->normal));

	b->surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	b->surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	b->surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * b->surface_p->d_zistepu -
			ycenter * b->surface_p->d_zistepv;

	b->surface_p++;
}


/*
================
R_RenderBmodelFace
================
*/
void R_RenderBmodelFace (rband_t *b, bedge_t *pedges, msurface_t *psurf)
{
	int			i, alpha;
	unsigned	mask;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	clipplane_t	*pclip;

	if (psurf->flags & SURF_NOTEXELS)
		return;
	alpha = R_SurfaceAlpha (b->entity, psurf);
	if (alpha < 256 || (psurf->flags & SURF_DRAWFENCE))
	{
		R_AddAfter (b, psurf, alpha);
		return;
	}

// skip out if no more surfs
	if (b->surface_p >= b->surf_max)
	{
		b->outofsurfaces = true;
		return;
	}

// ditto if not enough edges left
	if ((b->edge_p + psurf->numedges + 4) >= b->edge_max)
	{
		b->outofedges = true;
		return;
	}

	b->faceclip++;

// this is a dummy to give the caching mechanism someplace to write to
	b->pedge = &b->tedge;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (b->clipflags & mask)
		{
			b->clipplanes[i].next = pclip;
			pclip = &b->clipplanes[i];
		}
	}

// push the edges through
	b->emitted = false;
	b->nearzi = 0;
	b->nearzionly = false;
	b->makeleftedge = b->makerightedge = false;
// FIXME: keep clipped bmodel edges in clockwise order so last vertex caching
// can be used?
	b->lastvertvalid = false;

	for ( ; pedges ; pedges = pedges->pnext)
	{
		b->leftclipped = b->rightclipped = false;
		R_ClipEdge (b, pedges->v[0], pedges->v[1], pclip);

		if (b->leftclipped)
			b->makeleftedge = true;
		if (b->rightclipped)
			b->makerightedge = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (b->makeleftedge)
	{
		b->pedge = &b->tedge;
		R_ClipEdge (b, &b->leftexit, &b->leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right nearzi
	if (b->makerightedge)
	{
		b->pedge = &b->tedge;
		b->nearzionly = true;
		R_ClipEdge (b, &b->rightexit, &b->rightenter, b->clipplanes[1].next);
	}

// if no edges made it out, return without posting the surface
	if (!b->emitted)
		return;

	b->polycount++;

	b->surface_p->data = (void *)psurf;
	b->surface_p->nearzi = b->nearzi;
	b->surface_p->flags = psurf->flags;
	b->surface_p->insubmodel = true;
	b->surface_p->spanstate = 0;
	b->surface_p->entity = b->entity;
	b->surface_p->key = b->currentbkey;
	b->surface_p->spans = NULL;

	pplane = psurf->plane;
// FIXME: cache this?
	R_BandTransform (b, pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0f / (pplane->dist - DotProduct (b->modelorg, pplane->normal));

	b->surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	b->surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	b->surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * b->surface_p->d_zistepu -
			ycenter * b->surface_p->d_zistepv;

	b->surface_p++;
}
