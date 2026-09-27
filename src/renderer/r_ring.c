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
// r_ring.c -- rings lying on the floor, part of each lit: where an item is
// missing and how soon it is back (qualia's spawn rings, cl_items.c)
//
// A ring is a band of RING_SEGMENTS flat quads, those over the lit part more
// solid than the rest, the one the lit part ends in cut there. Each is clipped
// to the view, projected and filled in one color: depth tested and never
// depth written, so a ring is hidden by what stands in front of it and hides
// nothing.

#include "r_local.h"
#include "d_local.h"

#define	RING_SEGMENTS	48
#define	RING_WIDTH		0.11f		// the band's half width, of the radius
#define	RING_LIFT		1.0f		// above the floor, out of the floor's depth
#define	TRACK_ALPHA		26			// of 256: the part not lit, the floor reading through it
#define	FILL_ALPHA		115			// the lit part, still an annotation rather than paint

#define	RING_TAU		6.28318530718f

// the part of in on the front of plane, into out
static int R_ClipRing (vec3_t *in, int n, vec3_t *out, const clipplane_t *plane)
{
	float	d[MAXWORKINGVERTS], frac;
	int		i, j, k, m;

	for (i=0 ; i<n ; i++)
		d[i] = DotProduct (in[i], plane->normal) - plane->dist;
	for (i=m=0 ; i<n ; i++)
	{
		j = (i + 1) % n;
		if (d[i] >= 0)
		{
			VectorCopy (in[i], out[m]);		// a macro: its arguments are read three times
			m++;
		}
		if ((d[i] > 0 && d[j] < 0) || (d[i] < 0 && d[j] > 0))
		{
			frac = d[i] / (d[i] - d[j]);
			for (k=0 ; k<3 ; k++)
				out[m][k] = in[i][k] + frac * (in[j][k] - in[i][k]);
			m++;
		}
	}
	return m;
}

/*
================
R_DrawRingQuad

Clipped by the view planes in clipflags, those that cut the ring
================
*/
static void R_DrawRingQuad (vec3_t quad[4], pixel_t color, int alpha, int clipflags)
{
	vec3_t		a[MAXWORKINGVERTS], b[MAXWORKINGVERTS], *in, *out, *swap, local, transformed;
	emitpoint_t	verts[MAXWORKINGVERTS + 1];
	int			i, n;

	memcpy (a, quad, 4 * sizeof(vec3_t));
	in = a;
	out = b;
	n = 4;
	for (i=0 ; i<4 ; i++)
	{
		if (!(clipflags & (1 << i)))
			continue;
		n = R_ClipRing (in, n, out, &view_clipplanes[i]);
		if (n < 3)
			return;
		swap = in;
		in = out;
		out = swap;
	}

	for (i=0 ; i<n ; i++)
	{
		VectorSubtract (in[i], r_origin, local);
		TransformVector (local, transformed);
		if (transformed[2] < NEAR_CLIP)
			transformed[2] = (vec_t)NEAR_CLIP;
		verts[i].zi = 1.0f / transformed[2];
		verts[i].u = xcenter + xscale * verts[i].zi * transformed[0];
		verts[i].v = ycenter - yscale * verts[i].zi * transformed[1];
	}
	D_DrawFlatPolygon (verts, n, color, alpha);
}

// the cosine and sine of the ring's angle at segment s, fractions allowed
static void R_RingAngle (const r_ring_t *ring, float s, float cs[2])
{
	float	angle = ring->phase + s * RING_TAU / RING_SEGMENTS;

	cs[0] = cosf (angle);
	cs[1] = sinf (angle);
}

static void R_RingPoint (vec3_t out, const r_ring_t *ring, float radius, const float cs[2], float z)
{
	out[0] = ring->centre[0] + radius * cs[0];
	out[1] = ring->centre[1] + radius * cs[1];
	out[2] = z;
}

// the ring between two angles
static void R_DrawRingPart (const r_ring_t *ring, const float from[2], const float to[2], pixel_t color, int alpha,
	int clipflags)
{
	vec3_t	quad[4];
	float	inner, outer, z;

	inner = ring->radius * (1 - RING_WIDTH);
	outer = ring->radius * (1 + RING_WIDTH);
	z = ring->centre[2] + RING_LIFT;

	R_RingPoint (quad[0], ring, outer, from, z);
	R_RingPoint (quad[1], ring, outer, to, z);
	R_RingPoint (quad[2], ring, inner, to, z);
	R_RingPoint (quad[3], ring, inner, from, z);
	R_DrawRingQuad (quad, color, alpha, clipflags);
}

/*
================
R_RingClipFlags

The view planes that cut the ring, as bits; -1 if it is out of view. The
ring is taken as the sphere around it, a unit larger for its corners'
rounding.
================
*/
static int R_RingClipFlags (const r_ring_t *ring)
{
	vec3_t	centre;
	float	reach, d;
	int		i, clipflags = 0;

	VectorCopy (ring->centre, centre);
	centre[2] += RING_LIFT;
	reach = ring->radius * (1 + RING_WIDTH) + 1;
	for (i=0 ; i<4 ; i++)
	{
		d = DotProduct (centre, view_clipplanes[i].normal) - view_clipplanes[i].dist;
		if (d < -reach)
			return -1;
		if (d < reach)
			clipflags |= 1 << i;
	}
	return clipflags;
}

// an sRGB channel of a ring's color
static unsigned R_RingChannel (float c)
{
	return R_LightCode (R_SrgbToLinear (c < 0 ? 0 : c > 1 ? 1 : c));
}

/*
================
R_DrawRings

The scene's rings, after the translucent things and before the view model
================
*/
void R_DrawRings (void)
{
	const r_ring_t	*ring;
	pixel_t			color;
	float			lit, cs[RING_SEGMENTS + 1][2], end[2];
	int				i, s, clipflags;

	for (i=0, ring = r_scene.rings ; i<r_scene.numrings ; i++, ring++)
	{
		clipflags = R_RingClipFlags (ring);
		if (clipflags < 0)
			continue;
		color = RGB30 (R_RingChannel (ring->color[0]), R_RingChannel (ring->color[1]), R_RingChannel (ring->color[2]));
		lit = (ring->fill < 0 ? 0 : ring->fill > 1 ? 1 : ring->fill) * RING_SEGMENTS;
		for (s=0 ; s<=RING_SEGMENTS ; s++)
			R_RingAngle (ring, (float)s, cs[s]);
		for (s=0 ; s<RING_SEGMENTS ; s++)
		{
			if (s + 1 <= lit)
				R_DrawRingPart (ring, cs[s], cs[s + 1], color, FILL_ALPHA, clipflags);
			else if (s >= lit)
				R_DrawRingPart (ring, cs[s], cs[s + 1], color, TRACK_ALPHA, clipflags);
			else
			{	// where the lit part ends
				R_RingAngle (ring, lit, end);
				R_DrawRingPart (ring, cs[s], end, color, FILL_ALPHA, clipflags);
				R_DrawRingPart (ring, end, cs[s + 1], color, TRACK_ALPHA, clipflags);
			}
		}
	}
}
