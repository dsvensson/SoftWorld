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
// r_bsp.c

#include "r_local.h"
#include "r_local.h"

//
// current entity info, for what's drawn after the bands
//
entity_t		*currententity;
vec3_t			modelorg, base_modelorg;
								// modelorg is the viewpoint reletive to
								// the currently rendering entity
vec3_t			r_entorigin;	// the currently rendering entity in world
								// coordinates

static float			entity_rotation[3][3];

// A brush entity's polygon clipped through the world takes an edge and a
// vertex from a band's bedges and bverts for each node plane it crosses. A big
// entity crossing many nodes (Arcane Dimensions' maps) can outgrow them: the
// band is then drawn again with twice the room (R_GrowBandBModelClip), as its
// edge and surface lists are, up to a bound a map can't push it past.
#define MIN_BMODEL_VERTS	500
#define MIN_BMODEL_EDGES	1000
#define MAX_BMODEL_EDGES	(1 << 20)

static inline bool R_BandCullsBox (const rband_t *b, const float *minmaxs, const vec3_t offset, int clipflags);


//===========================================================================

/*
================
R_EntityRotate
================
*/
static void R_EntityRotate (float rotation[3][3], vec3_t vec)
{
	vec3_t	tvec;

	VectorCopy (vec, tvec);
	vec[0] = DotProduct (rotation[0], tvec);
	vec[1] = DotProduct (rotation[1], tvec);
	vec[2] = DotProduct (rotation[2], tvec);
}


/*
================
R_EntityRotation

A brush entity's angles as the rotation from the world into its model
================
*/
static void R_EntityRotation (const entity_t *ent, float rotation[3][3])
{
	float	angle, s, c, temp1[3][3], temp2[3][3], temp3[3][3];

// TODO: should use a look-up table
// TODO: should really be stored with the entity instead of being reconstructed
// TODO: could cache lazily, stored in the entity
// TODO: share work with R_SetUpAliasTransform

// yaw
	angle = ent->angles[YAW];		
	angle = (float)(angle * Q_PI*2 / 360);
	s = sinf(angle);
	c = cosf(angle);

	temp1[0][0] = c;
	temp1[0][1] = s;
	temp1[0][2] = 0;
	temp1[1][0] = -s;
	temp1[1][1] = c;
	temp1[1][2] = 0;
	temp1[2][0] = 0;
	temp1[2][1] = 0;
	temp1[2][2] = 1;


// pitch
	angle = ent->angles[PITCH];		
	angle = (float)(angle * Q_PI*2 / 360);
	s = sinf(angle);
	c = cosf(angle);

	temp2[0][0] = c;
	temp2[0][1] = 0;
	temp2[0][2] = -s;
	temp2[1][0] = 0;
	temp2[1][1] = 1;
	temp2[1][2] = 0;
	temp2[2][0] = s;
	temp2[2][1] = 0;
	temp2[2][2] = c;

	R_ConcatRotations (temp2, temp1, temp3);

// roll
	angle = ent->angles[ROLL];		
	angle = (float)(angle * Q_PI*2 / 360);
	s = sinf(angle);
	c = cosf(angle);

	temp1[0][0] = 1;
	temp1[0][1] = 0;
	temp1[0][2] = 0;
	temp1[1][0] = 0;
	temp1[1][1] = c;
	temp1[1][2] = s;
	temp1[2][0] = 0;
	temp1[2][1] = -s;
	temp1[2][2] = c;

	R_ConcatRotations (temp1, temp3, rotation);
}

/*
================
R_RotateBmodel

The view into currententity's model space
================
*/
void R_RotateBmodel (void)
{
	R_EntityRotation (currententity, entity_rotation);

//
// rotate modelorg and the transformation matrix
//
	R_EntityRotate (entity_rotation, modelorg);
	R_EntityRotate (entity_rotation, vpn);
	R_EntityRotate (entity_rotation, vright);
	R_EntityRotate (entity_rotation, vup);

	R_TransformFrustum ();
}

/*
================
R_EntityViewVectors

The view's vectors in a brush entity's model space, as R_RotateBmodel turns
them, for a thread that can't turn the view itself
================
*/
void R_EntityViewVectors (const entity_t *ent, vec3_t right, vec3_t up, vec3_t forward)
{
	float	rotation[3][3];

	R_EntityRotation (ent, rotation);
	VectorCopy (base_vright, right);
	VectorCopy (base_vup, up);
	VectorCopy (base_vpn, forward);
	R_EntityRotate (rotation, right);
	R_EntityRotate (rotation, up);
	R_EntityRotate (rotation, forward);
}

/*
================
R_EntityModelView

The view's origin and vectors in a brush entity's model space, as
R_DrawSurfaceAfter and R_RotateBmodel set modelorg and turn the view to it,
from the world's view; any thread's
================
*/
void R_EntityModelView (const entity_t *ent, vec3_t org, vec3_t right, vec3_t up, vec3_t forward)
{
	float	rotation[3][3];

	R_EntityRotation (ent, rotation);
	VectorSubtract (r_origin, ent->origin, org);
	R_EntityRotate (rotation, org);
	VectorCopy (base_vright, right);
	VectorCopy (base_vup, up);
	VectorCopy (base_vpn, forward);
	R_EntityRotate (rotation, right);
	R_EntityRotate (rotation, up);
	R_EntityRotate (rotation, forward);
}

/*
================
R_RotateBandBmodel

The band's view into its entity's model space
================
*/
void R_RotateBandBmodel (rband_t *b)
{
	R_EntityRotation (b->entity, b->entity_rotation);
	R_EntityRotate (b->entity_rotation, b->modelorg);
	R_EntityRotate (b->entity_rotation, b->vpn);
	R_EntityRotate (b->entity_rotation, b->vright);
	R_EntityRotate (b->entity_rotation, b->vup);
	R_TransformBandFrustum (b);
}


/*
================
R_RecursiveClipBPoly

A brush entity's face, or a piece of it, cut by the world's node planes into
the leaves it is in. A piece that doesn't reach past a plane on both sides
(ON_EPSILON) goes whole to one side, the one it is on, or for a piece in the
plane the back, where id's split puts one: that split took a vertex on the
plane for the back, so a piece in front with an edge on the plane came apart
into a piece with a clip edge running back over that edge and one of points,
and the face's span was left open on the lines by that edge, on to the
screen's edge (AD's ad_tfuma, a window rim of a func_breakable from the
start).
================
*/
static void R_RecursiveClipBPoly (rband_t *b, bedge_t *pedges, mnode_t *pnode, msurface_t *psurf)
{
	bedge_t		*psideedges[2], *pnextedge, *ptedge;
	int			i, side, lastside;
	float		dist, frac, lastdist;
	mplane_t	*splitplane, tplane;
	mvertex_t	*pvert, *plastvert, *ptvert;
	mnode_t		*pn;
	bool		front, back;

	psideedges[0] = psideedges[1] = NULL;

	b->makeclippededge = false;

// transform the BSP plane into model space
// FIXME: cache these?
	splitplane = pnode->plane;
	tplane.dist = splitplane->dist -
			DotProduct(b->entorigin, splitplane->normal);
	tplane.normal[0] = DotProduct (b->entity_rotation[0], splitplane->normal);
	tplane.normal[1] = DotProduct (b->entity_rotation[1], splitplane->normal);
	tplane.normal[2] = DotProduct (b->entity_rotation[2], splitplane->normal);

// whether the piece reaches past the plane on both sides (each vertex begins
// an edge)
	front = back = false;
	for (ptedge = pedges ; ptedge ; ptedge = ptedge->pnext)
	{
		dist = DotProduct (ptedge->v[0]->position, tplane.normal) - tplane.dist;
		front |= dist > ON_EPSILON;
		back |= dist < -ON_EPSILON;
	}
	if (!front || !back)
	{
		psideedges[front ? 0 : 1] = pedges;
		pedges = NULL;
	}

// clip edges to BSP plane
	for ( ; pedges ; pedges = pnextedge)
	{
		pnextedge = pedges->pnext;

	// set the status for the last point as the previous point
	// FIXME: cache this stuff somehow?
		plastvert = pedges->v[0];
		lastdist = DotProduct (plastvert->position, tplane.normal) -
				   tplane.dist;

		if (lastdist > 0)
			lastside = 0;
		else
			lastside = 1;

		pvert = pedges->v[1];

		dist = DotProduct (pvert->position, tplane.normal) - tplane.dist;

		if (dist > 0)
			side = 0;
		else
			side = 1;

		if (side != lastside)
		{
		// clipped
			if (b->numbverts >= b->maxbverts)
			{
				b->outofbmodel = true;
				return;
			}

		// generate the clipped vertex
			frac = lastdist / (lastdist - dist);
			ptvert = &b->bverts[b->numbverts++];
			ptvert->position[0] = plastvert->position[0] +
					frac * (pvert->position[0] -
					plastvert->position[0]);
			ptvert->position[1] = plastvert->position[1] +
					frac * (pvert->position[1] -
					plastvert->position[1]);
			ptvert->position[2] = plastvert->position[2] +
					frac * (pvert->position[2] -
					plastvert->position[2]);

		// split into two edges, one on each side, and remember entering
		// and exiting points
		// FIXME: share the clip edge by having a winding direction flag?
			if (b->numbedges >= b->maxbedges - 1)
			{
				b->outofbmodel = true;
				return;
			}

			ptedge = &b->bedges[b->numbedges];
			ptedge->pnext = psideedges[lastside];
			psideedges[lastside] = ptedge;
			ptedge->v[0] = plastvert;
			ptedge->v[1] = ptvert;

			ptedge = &b->bedges[b->numbedges + 1];
			ptedge->pnext = psideedges[side];
			psideedges[side] = ptedge;
			ptedge->v[0] = ptvert;
			ptedge->v[1] = pvert;

			b->numbedges += 2;

			if (side == 0)
			{
			// entering for front, exiting for back
				b->frontenter = ptvert;
				b->makeclippededge = true;
			}
			else
			{
				b->frontexit = ptvert;
				b->makeclippededge = true;
			}
		}
		else
		{
		// add the edge to the appropriate side
			pedges->pnext = psideedges[side];
			psideedges[side] = pedges;
		}
	}

// if anything was clipped, reconstitute and add the edges along the clip
// plane to both sides (but in opposite directions)
	if (b->makeclippededge)
	{
		if (b->numbedges >= b->maxbedges - 2)
		{
			b->outofbmodel = true;
			return;
		}

		ptedge = &b->bedges[b->numbedges];
		ptedge->pnext = psideedges[0];
		psideedges[0] = ptedge;
		ptedge->v[0] = b->frontexit;
		ptedge->v[1] = b->frontenter;

		ptedge = &b->bedges[b->numbedges + 1];
		ptedge->pnext = psideedges[1];
		psideedges[1] = ptedge;
		ptedge->v[0] = b->frontenter;
		ptedge->v[1] = b->frontexit;

		b->numbedges += 2;
	}

// draw or recurse further
	for (i=0 ; i<2 ; i++)
	{
		if (psideedges[i])
		{
		// draw if we've reached a non-solid leaf, done if all that's left is a
		// solid leaf, and continue down the tree if it's not a leaf
			pn = pnode->children[i];

		// we're done with this branch if the node or leaf isn't in the PVS
			if (pn->visframe == r_visframecount)
			{
				if (pn->contents < 0)
				{
					if (pn->contents != CONTENTS_SOLID)
					{
						b->currentbkey = b->leafkeys[(mleaf_t *)pn - r_scene.worldmodel->leafs];
						R_RenderBmodelFace (b, psideedges[i], psurf);
					}
				}
				else
				{
					R_RecursiveClipBPoly (b, psideedges[i], pnode->children[i],
									  psurf);
				}
			}
		}
	}
}


/*
================
R_GrowBandBModelClip

Twice the room for a band's clipping of brush entities, after it ran out, or
the first; false at the bound (the band is drawn without what didn't fit)
================
*/
bool R_GrowBandBModelClip (rband_t *b)
{
	static bool	warned;

	if (b->maxbedges >= MAX_BMODEL_EDGES)
	{
		if (!warned)
			Con_Printf ("A brush entity needs more than %i edges to draw\n", MAX_BMODEL_EDGES);
		warned = true;
		return false;
	}
	Mem_Free (b->bverts);
	Mem_Free (b->bedges);
	b->maxbverts = b->maxbverts ? b->maxbverts * 2 : MIN_BMODEL_VERTS;
	b->maxbedges = b->maxbedges ? b->maxbedges * 2 : MIN_BMODEL_EDGES;
	b->bverts = Mem_Alloc ((size_t)b->maxbverts * sizeof(*b->bverts));
	b->bedges = Mem_Alloc ((size_t)b->maxbedges * sizeof(*b->bedges));
	return true;
}

/*
================
R_DrawSolidClippedSubmodelPolygons
================
*/
void R_DrawSolidClippedSubmodelPolygons (rband_t *b, model_t *pmodel)
{
	int			i, j, lindex;
	vec_t		dot;
	msurface_t	*psurf;
	int			numsurfaces;
	mplane_t	*pplane;
	bedge_t		*pbedge;
	medge_t		*pedge, *pedges;

// FIXME: use bounding-box-based frustum clipping info?

	psurf = &pmodel->surfaces[pmodel->firstmodelsurface];
	numsurfaces = pmodel->nummodelsurfaces;
	pedges = pmodel->edges;

	for (i=0 ; i<numsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (b->modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
		// FIXME: use bounding-box-based frustum clipping info?

		// copy the edges to bedges, flipping if necessary so always
		// clockwise winding
		// FIXME: if edges and vertices get caches, these assignments must move
		// outside the loop, and overflow checking must be done here
		// a face of an entity that isn't turned is culled as the world's
			if (b->cullflags && !b->entity->angles[0] && !b->entity->angles[1] && !b->entity->angles[2]
				&& R_BandCullsBox (b, psurf->minmaxs, b->entorigin, b->cullflags))
				continue;

			b->numbverts = b->numbedges = 0;
			if (psurf->numedges > b->maxbedges)
			{
				b->outofbmodel = true;
				continue;
			}

			if (psurf->numedges > 0)		// the loader refuses faces without
			{
				pbedge = &b->bedges[b->numbedges];
				b->numbedges += psurf->numedges;

				for (j=0 ; j<psurf->numedges ; j++)
				{
				   lindex = pmodel->surfedges[psurf->firstedge+j];

					if (lindex > 0)
					{
						pedge = &pedges[lindex];
						pbedge[j].v[0] = &b->vertbase[pedge->v[0]];
						pbedge[j].v[1] = &b->vertbase[pedge->v[1]];
					}
					else
					{
						lindex = -lindex;
						pedge = &pedges[lindex];
						pbedge[j].v[0] = &b->vertbase[pedge->v[1]];
						pbedge[j].v[1] = &b->vertbase[pedge->v[0]];
					}

					pbedge[j].pnext = &pbedge[j+1];
				}

				pbedge[j-1].pnext = NULL;	// mark end of edges

				R_RecursiveClipBPoly (b, pbedge, b->entity->topnode, psurf);
			}
		}
	}
}


/*
================
R_DrawSubmodelPolygons
================
*/
void R_DrawSubmodelPolygons (rband_t *b, model_t *pmodel, int clipflags)
{
	int			i;
	vec_t		dot;
	msurface_t	*psurf;
	int			numsurfaces;
	mplane_t	*pplane;

// FIXME: use bounding-box-based frustum clipping info?

	psurf = &pmodel->surfaces[pmodel->firstmodelsurface];
	numsurfaces = pmodel->nummodelsurfaces;

	for (i=0 ; i<numsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (b->modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			b->currentkey = b->leafkeys[(mleaf_t *)b->entity->topnode - r_scene.worldmodel->leafs];

		// FIXME: use bounding-box-based frustum clipping info?
			R_RenderFace (b, psurf, clipflags);
		}
	}
}


/*
================
R_BandCullsBox

Whether a box, minmaxs as a node's moved by offset, is wholly beyond one of
the band's cull planes in clipflags: none of it can be on the band's lines.
Faces are small beside the nodes above them, so most a band would clip are
culled here. (Not a brush model's pieces by the nodes they're clipped into: a
node's bounds are its world's, which an entity can be outside of.)
================
*/
static inline bool R_BandCullsBox (const rband_t *b, const float *minmaxs, const vec3_t offset, int clipflags)
{
	int			i;
	const int	*pindex;
	vec3_t		rejectpt;

	for (i=0 ; i<2 ; i++)
	{
		if (!(clipflags & (16 << i)))
			continue;
		pindex = b->cullindexes[i];
		rejectpt[0] = minmaxs[pindex[0]] + offset[0];
		rejectpt[1] = minmaxs[pindex[1]] + offset[1];
		rejectpt[2] = minmaxs[pindex[2]] + offset[2];
		if (DotProduct (rejectpt, b->cullplanes[i].normal) - b->cullplanes[i].dist <= 0)
			return true;
	}
	return false;
}

/*
================
R_BandOccluded

Whether all a box could be on the band's lines is on pixels covered by the
faces the walk met (rband_t cover): its corners in view space bound by the
view's axes, and their projections by the bounds' extremes, a pixel out.
Not where any of it is nearer than the near clip.
================
*/
static bool R_BandOccluded (const rband_t *b, const float *minmaxs)
{
	vec3_t			center, half;
	float			cx, cy, cz, ex, ey, ez, zmin, zmax, xmin, xmax, ymin, ymax;
	float			minxz, maxxz, minyz, maxyz;
	int				x0, x1, y0, y1, y, w, w0, w1, j;
	const uint64_t	*row;
	uint64_t		mask;

	for (j = 0 ; j < 3 ; j++)
	{
		center[j] = 0.5f * (minmaxs[j] + minmaxs[3+j]) - b->modelorg[j];
		half[j] = 0.5f * (minmaxs[3+j] - minmaxs[j]);
	}
	cz = DotProduct (center, b->vpn);
	ez = fabsf (b->vpn[0]) * half[0] + fabsf (b->vpn[1]) * half[1] + fabsf (b->vpn[2]) * half[2];
	zmin = cz - ez;
	if (!(zmin >= NEAR_CLIP * 2))
		return false;		// near the eye, or NaN
	zmax = cz + ez;
	cx = DotProduct (center, b->vright);
	ex = fabsf (b->vright[0]) * half[0] + fabsf (b->vright[1]) * half[1] + fabsf (b->vright[2]) * half[2];
	cy = DotProduct (center, b->vup);
	ey = fabsf (b->vup[0]) * half[0] + fabsf (b->vup[1]) * half[1] + fabsf (b->vup[2]) * half[2];
	xmin = cx - ex;
	xmax = cx + ex;
	ymin = cy - ey;
	ymax = cy + ey;
	minxz = xmin >= 0 ? xmin / zmax : xmin / zmin;
	maxxz = xmax >= 0 ? xmax / zmin : xmax / zmax;
	minyz = ymin >= 0 ? ymin / zmax : ymin / zmin;
	maxyz = ymax >= 0 ? ymax / zmin : ymax / zmax;

	x0 = (int)floorf (xcenter + xscale * minxz) - 1;
	x1 = (int)ceilf (xcenter + xscale * maxxz) + 1;
	y0 = (int)floorf (ycenter - yscale * maxyz) - 1;
	y1 = (int)ceilf (ycenter - yscale * minyz) + 1;
	if (x0 < r_refdef.vrect.x)
		x0 = r_refdef.vrect.x;
	if (x1 > r_refdef.vrectright - 1)
		x1 = r_refdef.vrectright - 1;
	if (y0 < b->top)
		y0 = b->top;
	if (y1 > b->bottom - 1)
		y1 = b->bottom - 1;
	if (x0 > x1 || y0 > y1)
		return true;		// none of it on the band's lines

	w0 = x0 >> 6;
	w1 = x1 >> 6;
	for (y = y0 ; y <= y1 ; y++)
	{
		row = b->cover + (size_t)y * (size_t)b->coverwords;
		for (w = w0 ; w <= w1 ; w++)
		{
			mask = ~(uint64_t)0;
			if (w == w0)
				mask &= ~(uint64_t)0 << (x0 & 63);
			if (w == w1)
				mask &= ~(uint64_t)0 >> (63 - (x1 & 63));
			if ((row[w] & mask) != mask)
				return false;
		}
	}
	return true;
}

/*
================
R_OccludedKeys

The leaves under a hidden node, one key for them all, where the walk would
have given them theirs: a brush entity in one is split there and sorted by
it (R_RecursiveClipBPoly), behind what hides the node
================
*/
static void R_OccludedKeys (rband_t *b, mnode_t *node, int key)
{
	while (node->contents != CONTENTS_SOLID && node->visframe == r_visframecount)
	{
		if (node->contents < 0)
		{
			b->leafkeys[(mleaf_t *)node - r_scene.worldmodel->leafs] = key;
			return;
		}
		R_OccludedKeys (b, node->children[0], key);
		node = node->children[1];
	}
}

/*
================
R_RecursiveWorldNode
================
*/
static void R_RecursiveWorldNode (rband_t *b, mnode_t *node, int clipflags)
{
	int			i, c, n, side, *pindex;
	vec3_t		acceptpt, rejectpt;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	const float	*normal;
	float		dist;
	double		d, dot;

	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;

// cull the clipping planes if not trivial accept; bits 4 and 5 are the band's
// cull planes, only ever culled against
// FIXME: the compiler is doing a lousy job of optimizing here; it could be
//  twice as fast in ASM
	if (clipflags)
	{
		for (i=0 ; i<6 ; i++)
		{
			if (! (clipflags & (1<<i)) )
				continue;	// don't need to clip against it

		// generate accept and reject points
		// FIXME: do with fast look-ups or integer tests based on the sign bit
		// of the floating point values

			pindex = i < 4 ? pfrustum_indexes[i] : b->cullindexes[i - 4];
			normal = i < 4 ? view_clipplanes[i].normal : b->cullplanes[i - 4].normal;
			dist = i < 4 ? view_clipplanes[i].dist : b->cullplanes[i - 4].dist;

			rejectpt[0] = (float)node->minmaxs[pindex[0]];
			rejectpt[1] = (float)node->minmaxs[pindex[1]];
			rejectpt[2] = (float)node->minmaxs[pindex[2]];
			
			d = DotProduct (rejectpt, normal);
			d -= dist;

			if (d <= 0)
				return;

			acceptpt[0] = (float)node->minmaxs[pindex[3+0]];
			acceptpt[1] = (float)node->minmaxs[pindex[3+1]];
			acceptpt[2] = (float)node->minmaxs[pindex[3+2]];

			d = DotProduct (acceptpt, normal);
			d -= dist;

			if (d >= 0)
				clipflags &= ~(1<<i);	// node is entirely on screen
		}
	}

// hidden behind the faces met already
	if (b->covered && b->occlusion && R_BandOccluded (b, node->minmaxs))
	{
		R_OccludedKeys (b, node, b->currentkey);
		b->currentkey++;
		return;
	}
	
// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

	// its surfaces may be drawn where the band meets them on nodes
		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				n = (int)(*mark - r_scene.worldmodel->surfaces);
				b->surfvisible[n >> 3] |= (byte)(1 << (n & 7));
				mark++;
			} while (--c);
		}

		b->leafkeys[pleaf - r_scene.worldmodel->leafs] = b->currentkey;
		b->currentkey++;		// all bmodels in a leaf share the same key
	}
	else
	{
	// node is just a decision point, so go down the apropriate sides

	// find which side of the node we are on
		plane = node->plane;

		switch (plane->type)
		{
		case PLANE_X:
			dot = b->modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = b->modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = b->modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct (b->modelorg, plane->normal) - plane->dist;
			break;
		}
	
		if (dot >= 0)
			side = 0;
		else
			side = 1;

	// recurse down the children, front side first
		R_RecursiveWorldNode (b, node->children[side], clipflags);

	// draw stuff
		c = node->numsurfaces;

		if (c)
		{
			n = (int)node->firstsurface;
			surf = r_scene.worldmodel->surfaces + n;

			if (dot < -BACKFACE_EPSILON)
			{
				do
				{
					if ((surf->flags & SURF_PLANEBACK) &&
						(b->surfvisible[n >> 3] & (1 << (n & 7))) &&
						!((clipflags & 48) && R_BandCullsBox (b, surf->minmaxs, vec3_origin, clipflags)) &&
						!(b->covered && b->occlusion && R_BandOccluded (b, surf->minmaxs)))
					{
						R_RenderFace (b, surf, clipflags);
					}

					surf++;
					n++;
				} while (--c);
			}
			else if (dot > BACKFACE_EPSILON)
			{
				do
				{
					if (!(surf->flags & SURF_PLANEBACK) &&
						(b->surfvisible[n >> 3] & (1 << (n & 7))) &&
						!((clipflags & 48) && R_BandCullsBox (b, surf->minmaxs, vec3_origin, clipflags)) &&
						!(b->covered && b->occlusion && R_BandOccluded (b, surf->minmaxs)))
					{
						R_RenderFace (b, surf, clipflags);
					}

					surf++;
					n++;
				} while (--c);
			}

		// all surfaces on the same node share the same sequence number
			b->currentkey++;
		}

	// recurse down the back side
		R_RecursiveWorldNode (b, node->children[!side], clipflags);
	}
}



/*
================
R_RenderWorld

The world's faces on the band's lines; its cull planes are needed where it
doesn't reach the view's top or bottom
================
*/
void R_RenderWorld (rband_t *b)
{
	model_t		*clmodel;

	b->entity = &r_worldentity;
	b->insubmodel = false;
	VectorCopy (r_origin, b->modelorg);
	clmodel = b->entity->model;
	b->vertbase = clmodel->vertexes;

	R_RecursiveWorldNode (b, clmodel->nodes, 15 | b->cullflags);
}


/*
=============
R_BmodelCheckBBox

The view planes a brush entity crosses, which its faces are clipped to, or
BMODEL_FULLY_CLIPPED if it's wholly outside them, or outside a band's cull
planes when b is given
=============
*/
int R_BmodelCheckBBox (const entity_t *ent, const float *minmaxs, const rband_t *b)
{
	int			i, clipflags, planes;
	const int	*pindex;
	vec3_t		acceptpt, rejectpt;
	const float	*normal;
	float		dist;
	double		d;

	clipflags = 0;
	planes = b ? 4 + 2 : 4;

	if (ent->angles[0] || ent->angles[1]
		|| ent->angles[2])
	{
		for (i=0 ; i<planes ; i++)
		{
			if (i >= 4 && !(b->cullflags & (1 << i)))
				continue;
			normal = i < 4 ? view_clipplanes[i].normal : b->cullplanes[i - 4].normal;
			dist = i < 4 ? view_clipplanes[i].dist : b->cullplanes[i - 4].dist;

			d = DotProduct (ent->origin, normal);
			d -= dist;

			if (d <= -ent->model->radius)
				return BMODEL_FULLY_CLIPPED;

			if (d <= ent->model->radius && i < 4)
				clipflags |= (1<<i);
		}
	}
	else
	{
		for (i=0 ; i<planes ; i++)
		{
			if (i >= 4 && !(b->cullflags & (1 << i)))
				continue;
			pindex = i < 4 ? pfrustum_indexes[i] : b->cullindexes[i - 4];
			normal = i < 4 ? view_clipplanes[i].normal : b->cullplanes[i - 4].normal;
			dist = i < 4 ? view_clipplanes[i].dist : b->cullplanes[i - 4].dist;

		// generate accept and reject points
		// FIXME: do with fast look-ups or integer tests based on the sign bit
		// of the floating point values

			rejectpt[0] = minmaxs[pindex[0]];
			rejectpt[1] = minmaxs[pindex[1]];
			rejectpt[2] = minmaxs[pindex[2]];
			
			d = DotProduct (rejectpt, normal);
			d -= dist;

			if (d <= 0)
				return BMODEL_FULLY_CLIPPED;

			acceptpt[0] = minmaxs[pindex[3+0]];
			acceptpt[1] = minmaxs[pindex[3+1]];
			acceptpt[2] = minmaxs[pindex[3+2]];

			d = DotProduct (acceptpt, normal);
			d -= dist;

			if (d <= 0 && i < 4)
				clipflags |= (1<<i);
		}
	}

	return clipflags;
}


/*
=============
R_DrawBEntities

The faces of the brush entities R_PrepareBrushEntities gave a topnode to, on
the band's lines: those crossing the world's nodes are clipped through them
to the leaves they fall in
=============
*/
void R_DrawBEntities (rband_t *b)
{
	int			i, j, clipflags;
	entity_t	*ent;
	model_t		*clmodel;
	float		minmaxs[6];

	b->insubmodel = true;

	for (i=0 ; i<(*r_scene.numvisedicts) ; i++)
	{
		ent = &r_scene.visedicts[i];
		if (ent->model->type != mod_brush || !ent->topnode)
			continue;

		clmodel = ent->model;

	// see if the bounding box lets us trivially reject, also sets
	// trivial accept status
		for (j=0 ; j<3 ; j++)
		{
			minmaxs[j] = ent->origin[j] +
					clmodel->mins[j];
			minmaxs[3+j] = ent->origin[j] +
					clmodel->maxs[j];
		}

		clipflags = R_BmodelCheckBBox (ent, minmaxs, b);
		if (clipflags == BMODEL_FULLY_CLIPPED)
			continue;

		b->entity = ent;
		VectorCopy (ent->origin, b->entorigin);
		VectorSubtract (r_origin, b->entorigin, b->modelorg);
		b->vertbase = clmodel->vertexes;

	// FIXME: stop transforming twice
		R_RotateBandBmodel (b);

		if (ent->topnode->contents >= 0)
		{
		// not a leaf; has to be clipped to the world BSP
			b->clipflags = clipflags;
			R_DrawSolidClippedSubmodelPolygons (b, clmodel);
		}
		else
		{
		// falls entirely in one leaf, so we just put all the
		// edges in the edge list and let 1/z sorting handle
		// drawing order
			R_DrawSubmodelPolygons (b, clmodel, clipflags);
		}

	// put back world rotation and frustum clipping
		R_BandWorldView (b);
	}

	b->insubmodel = false;
}
