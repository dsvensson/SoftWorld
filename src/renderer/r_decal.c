/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

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
// r_decal.c -- decals cut to a brush model's faces, as FTE cuts them: a box
// round the decal's centre, its sides along the decal's normal and tangents,
// cuts each face the model's nodes find near it, and what is left of the faces
// is the decal's triangles

#include "r_local.h"

#define	DECAL_MAXVERTS	(128 * 3)
#define	DECAL_EPSILON	(1.0f / 32)

typedef struct
{
	vec3_t	center;
	vec3_t	normal;
	vec3_t	planenormal[6];
	float	planedist[6];
	float	radius;
	bool	facing;
	void	(*callback) (void *ctx, const vec3_t *points, int numtris);
	void	*ctx;
} decalclip_t;

// what of in is in front of the plane, into out
static int R_DecalClipPlane (const vec3_t *in, vec3_t *out, int n, const float *normal, float dist)
{
	enum {CUT, KEPT, ON};
	float	dot[DECAL_MAXVERTS + 1], frac;
	byte	keep[DECAL_MAXVERTS + 1];
	int		i, k, m, cut = 0;

	for (i = 0 ; i < n ; i++)
	{
		dot[i] = DotProduct (in[i], normal) - dist;
		keep[i] = dot[i] < -DECAL_EPSILON ? CUT : dot[i] > DECAL_EPSILON ? KEPT : ON;
		cut += keep[i] == CUT;
	}
	dot[n] = dot[0];
	keep[n] = keep[0];
	if (cut == n)
		return 0;
	if (!cut)
	{
		memcpy (out, in, sizeof(vec3_t) * (size_t)n);
		return n;
	}

	for (i = m = 0 ; i < n ; i++)
	{
		if (keep[i] != CUT)
		{
			VectorCopy (in[i], out[m]);
			m++;
			if (keep[i] == ON)
				continue;
		}
		if (keep[i+1] == ON || keep[i] == keep[i+1])
			continue;
		frac = dot[i] - dot[i+1];
		frac = frac ? dot[i] / frac : 0;
		for (k = 0 ; k < 3 ; k++)
			out[m][k] = in[i][k] + frac * (in[(i + 1) % n][k] - in[i][k]);
		m++;
	}
	return m;
}

// a face's polygon cut by the box, as triangles to the callback
static void R_DecalClipPolygon (decalclip_t *dc, const vec3_t *verts, int n)
{
	vec3_t	poly[2][DECAL_MAXVERTS], tris[DECAL_MAXVERTS], d1, d2, cr;
	int		p, flip, numtris;

	if (dc->facing)
	{
		// the face's own way, by its winding: nothing seen nearly edge on
		VectorSubtract (verts[1], verts[0], d1);
		for (p = 2 ; ; p++)
		{
			if (p >= n)
				return;
			VectorSubtract (verts[p], verts[0], d2);
			CrossProduct (d1, d2, cr);
			if (DotProduct (cr, cr) > 0.1f)
				break;
		}
		VectorNormalize (cr);
		if (DotProduct (cr, dc->normal) < 0.1f)
			return;
	}

	n = R_DecalClipPlane (verts, poly[0], n, dc->planenormal[0], dc->planedist[0]);
	for (p = 1, flip = 0 ; p < 6 && n >= 3 ; p++, flip ^= 1)
		n = R_DecalClipPlane ((const vec3_t *)poly[flip], poly[flip^1], n, dc->planenormal[p], dc->planedist[p]);
	if (n < 3)
		return;

	// a fan of triangles
	for (numtris = 0 ; n-- > 2 && (numtris + 1) * 3 <= DECAL_MAXVERTS ; numtris++)
	{
		VectorCopy (poly[flip][0], tris[numtris*3]);
		VectorCopy (poly[flip][n-1], tris[numtris*3+1]);
		VectorCopy (poly[flip][n], tris[numtris*3+2]);
	}
	if (numtris)
		dc->callback (dc->ctx, (const vec3_t *)tris, numtris);
}

// a face's polygon, from its edges
static void R_DecalClipSurface (const model_t *mod, decalclip_t *dc, const msurface_t *surf)
{
	vec3_t	verts[DECAL_MAXVERTS];
	int		i, e;

	if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
		return;
	if (surf->numedges > DECAL_MAXVERTS - 8)	// room for the cuts' new corners
		return;
	for (i = 0 ; i < surf->numedges ; i++)
	{
		e = mod->surfedges[surf->firstedge + i];
		e = e >= 0 ? (int)mod->edges[e].v[0] : (int)mod->edges[-e].v[1];
		VectorCopy (mod->vertexes[e].position, verts[i]);
	}
	R_DecalClipPolygon (dc, (const vec3_t *)verts, surf->numedges);
}

static void R_DecalClipNode (const model_t *mod, decalclip_t *dc, const mnode_t *node)
{
	const msurface_t	*surf;
	float				dist, facing;
	unsigned			i;

	while (node->contents >= 0)
	{
		dist = DotProduct (dc->center, node->plane->normal) - node->plane->dist;
		if (dist > dc->radius)
		{
			node = node->children[0];
			continue;
		}
		if (dist < -dc->radius)
		{
			node = node->children[1];
			continue;
		}

		surf = mod->surfaces + node->firstsurface;
		for (i = 0 ; i < node->numsurfaces ; i++, surf++)
		{
			if (dc->facing)
			{
				facing = DotProduct (surf->plane->normal, dc->normal);
				if ((surf->flags & SURF_PLANEBACK ? -facing : facing) > -0.5f)
					continue;
			}
			R_DecalClipSurface (mod, dc, surf);
		}
		R_DecalClipNode (mod, dc, node->children[0]);
		node = node->children[1];
	}
}

/*
=================
R_ClipDecal

A decal's triangles on a brush model's faces (not skies or liquids): what of
them a box size across round center leaves, its sides along normal and the
tangents, to the callback a few at a time. Facing, only faces facing mostly
against normal.
=================
*/
void R_ClipDecal (struct model_s *mod, const vec3_t center, const vec3_t normal, const vec3_t tangent1,
	const vec3_t tangent2, float size, bool facing, void (*callback) (void *ctx, const vec3_t *points, int numtris),
	void *ctx)
{
	decalclip_t	dc;
	float		r;
	int			p;

	if (!mod || mod->type != mod_brush || !mod->nodes)
		return;
	VectorCopy (center, dc.center);
	VectorCopy (normal, dc.normal);
	dc.radius = 0;
	dc.facing = facing;
	dc.callback = callback;
	dc.ctx = ctx;
	for (p = 0 ; p < 3 ; p++)
	{
		const float	*axis = p == 0 ? tangent1 : p == 1 ? tangent2 : normal;

		VectorCopy (axis, dc.planenormal[p*2]);
		VectorScale (dc.planenormal[p*2], -1, dc.planenormal[p*2+1]);
	}
	for (p = 0 ; p < 6 ; p++)
	{
		r = sqrtf (DotProduct (dc.planenormal[p], dc.planenormal[p]));
		if (!r)
			return;
		VectorScale (dc.planenormal[p], 1 / r, dc.planenormal[p]);
		r *= size / 2;
		dc.radius = fmaxf (dc.radius, r);
		dc.planedist[p] = DotProduct (dc.center, dc.planenormal[p]) - r;
	}
	R_DecalClipNode (mod, &dc, mod->nodes + mod->firstnode);
}
