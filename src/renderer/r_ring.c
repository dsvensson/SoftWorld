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
================
*/
static void R_DrawRingQuad (vec3_t quad[4], pixel_t color, int alpha)
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

static void R_RingPoint (vec3_t out, const r_ring_t *ring, float radius, float angle, float z)
{
	out[0] = ring->centre[0] + radius * cosf (angle);
	out[1] = ring->centre[1] + radius * sinf (angle);
	out[2] = z;
}

// the ring from segment from to segment to, fractions allowed
static void R_DrawRingPart (const r_ring_t *ring, float from, float to, pixel_t color, int alpha)
{
	vec3_t	quad[4];
	float	a0, a1, inner, outer, z;

	a0 = ring->phase + from * RING_TAU / RING_SEGMENTS;
	a1 = ring->phase + to * RING_TAU / RING_SEGMENTS;
	inner = ring->radius * (1 - RING_WIDTH);
	outer = ring->radius * (1 + RING_WIDTH);
	z = ring->centre[2] + RING_LIFT;

	R_RingPoint (quad[0], ring, outer, a0, z);
	R_RingPoint (quad[1], ring, outer, a1, z);
	R_RingPoint (quad[2], ring, inner, a1, z);
	R_RingPoint (quad[3], ring, inner, a0, z);
	R_DrawRingQuad (quad, color, alpha);
}

static unsigned R_RingChannel (float c)
{
	c *= RGB30_WHITE;
	return c <= 0 ? 0 : c >= 1023 ? 1023 : (unsigned)c;
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
	float			lit;
	int				i, s;

	for (i=0, ring = r_scene.rings ; i<r_scene.numrings ; i++, ring++)
	{
		color = RGB30 (R_RingChannel (ring->color[0]), R_RingChannel (ring->color[1]), R_RingChannel (ring->color[2]));
		lit = (ring->fill < 0 ? 0 : ring->fill > 1 ? 1 : ring->fill) * RING_SEGMENTS;
		for (s=0 ; s<RING_SEGMENTS ; s++)
		{
			if (s + 1 <= lit)
				R_DrawRingPart (ring, (float)s, s + 1.0f, color, FILL_ALPHA);
			else if (s >= lit)
				R_DrawRingPart (ring, (float)s, s + 1.0f, color, TRACK_ALPHA);
			else
			{	// where the lit part ends
				R_DrawRingPart (ring, (float)s, lit, color, FILL_ALPHA);
				R_DrawRingPart (ring, lit, s + 1.0f, color, TRACK_ALPHA);
			}
		}
	}
}
