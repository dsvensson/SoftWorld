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
// d_polyspan.c: the spans of a convex polygon on the screen, which sprites,
// fences, translucent faces and flat polygons are drawn from (d_sprite.c's
// scanners, apart so tests/test_polyspan.c can run them alone)

#include "r_local.h"
#include "d_local.h"


/*
=====================
D_ScanLeftEdge
=====================
*/
static void D_ScanLeftEdge (emitpoint_t *pverts, int nump, int minindex, int maxindex, sspan_t *spans)
{
	int			i, v, itop, ibottom, lmaxindex;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope;
	fixed16_t	u, u_step;

	pspan = spans;
	i = minindex;
	if (i == 0)
		i = nump;

	lmaxindex = maxindex;
	if (lmaxindex == 0)
		lmaxindex = nump;

	vtop = ceilf(pverts[i].v);

	do
	{
		pvert = &pverts[i];
		pnext = pvert - 1;

		vbottom = ceilf(pnext->v);

		if (vtop < vbottom)
		{
			du = pnext->u - pvert->u;
			dv = pnext->v - pvert->v;
			slope = du / dv;
			// a nearly level edge's slope is past an int's range: saturated,
			// and the steps wrap
			u_step = R_SaturateInt (slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((unsigned)R_SaturateInt ((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
					(0x10000 - 1));
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->u = u >> 16;
				pspan->v = v;
				u = (int)((unsigned)u + (unsigned)u_step);
				pspan++;
			}
		}

		vtop = vbottom;

		i--;
		if (i == 0)
			i = nump;

	} while (i != lmaxindex);
}


/*
=====================
D_ScanRightEdge
=====================
*/
static void D_ScanRightEdge (emitpoint_t *pverts, int nump, int minindex, int maxindex, sspan_t *spans)
{
	int			i, v, itop, ibottom;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope;
	fixed16_t	u, u_step;

	pspan = spans;
	i = minindex;

	vtop = ceilf(pverts[i].v);

	do
	{
		pvert = &pverts[i];
		pnext = pvert + 1;

		vbottom = ceilf(pnext->v);

		if (vtop < vbottom)
		{
			du = pnext->u - pvert->u;
			dv = pnext->v - pvert->v;
			slope = du / dv;
			// as the left edge's
			u_step = R_SaturateInt (slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((unsigned)R_SaturateInt ((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
					(0x10000 - 1));
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->count = (u >> 16) - pspan->u;
				u = (int)((unsigned)u + (unsigned)u_step);
				pspan++;
			}
		}

		vtop = vbottom;

		i++;
		if (i == nump)
			i = 0;

	} while (i != maxindex);

	pspan->count = DS_SPAN_LIST_END;	// mark the end of the span list
}


/*
=====================
D_ClampToRect

x within lo..hi, NaN to lo
=====================
*/
static float D_ClampToRect (float x, float lo, float hi)
{
	return x >= lo ? (x <= hi ? x : hi) : lo;
}

/*
=====================
D_PolygonSpans

The vertices are first held to rect, half a pixel past its edges as R_EmitEdge
holds the world's, for both edges to scan the same lines in it: the clipping
to the view's planes leaves a corner near the eye a pixel or more outside, and
the left edge, which id didn't hold, began a line above the right's, its
spans written before the screen and its 1/z read 16 GB past the depth buffer
(an access violation from a translucent face against the eye).
=====================
*/
bool D_PolygonSpans (emitpoint_t *pverts, int nump, sspan_t *spans, const vrect_t *rect)
{
	int			i, minindex, maxindex;
	float		ymin, ymax, left, right, top, bottom;

	left = (float)rect->x - 0.5f;
	right = (float)(rect->x + rect->width) - 0.5f;
	top = (float)rect->y - 0.5f;
	bottom = (float)(rect->y + rect->height) - 0.5f;

// find the top and bottom vertices, and make sure there's at least one scan to
// draw
	ymin = 999999.9f;
	ymax = -999999.9f;
	minindex = maxindex = 0;
	for (i=0 ; i<nump ; i++)
	{
		pverts[i].u = D_ClampToRect (pverts[i].u, left, right);
		pverts[i].v = D_ClampToRect (pverts[i].v, top, bottom);
		if (pverts[i].v < ymin)
		{
			ymin = pverts[i].v;
			minindex = i;
		}
		if (pverts[i].v > ymax)
		{
			ymax = pverts[i].v;
			maxindex = i;
		}
	}

	ymin = ceilf(ymin);
	ymax = ceilf(ymax);
	if (ymin >= ymax)
		return false;		// doesn't cross any scans at all

// copy the first vertex to the last vertex, so we don't have to deal with
// wrapping
	pverts[nump] = pverts[0];

	D_ScanLeftEdge (pverts, nump, minindex, maxindex, spans);
	D_ScanRightEdge (pverts, nump, minindex, maxindex, spans);
	return true;
}
