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
// r_band.c -- the bands of the view (r_local.h): laid out, given room, run
// on the worker threads, and run again with more room when they run out

#include "r_local.h"
#include "d_local.h"

rband_t		r_bands[MAX_BANDS];
int			r_numbands;

static int	r_edgewidth, r_edgeheight;	// the frame buffer's, R_SetEdgeSize
static int	r_bandedges, r_bandsurfs;	// the room a band starts a map with

static int	r_pending[MAX_BANDS];		// the bands to run
static int	r_numpending;

// what drawing each of the view's lines took lately, in seconds: the bands are
// laid out to take as long as each other, as where the world is busy (the
// horizon of a big map) takes much more than where it isn't
static float	*r_linecost;		// the frame buffer's height of them
static vrect_t	r_linecostview;		// the view they're for

#define MIN_AFTERS		64
#define MIN_BAND_LINES	4		// thinner bands walk the world's top again for little

/*
==============
R_SetEdgeSize

The frame buffer's size, which the bands' lines and columns are for
==============
*/
void R_SetEdgeSize (int width, int height)
{
	r_edgewidth = width;
	r_edgeheight = height;

	Mem_Free (r_linecost);
	r_linecost = Mem_Alloc ((size_t)height * sizeof(*r_linecost));
	r_linecostview.width = 0;		// not measured yet
}

/*
==============
R_SetBandRoom

The edges and surfaces each band starts a map with; the bands' are let go,
and their world's arrays
==============
*/
void R_SetBandRoom (int numedges, int numsurfs)
{
	rband_t	*b;

	r_bandedges = numedges > MINEDGES ? numedges : MINEDGES;
	r_bandsurfs = numsurfs > MINSURFACES ? numsurfs : MINSURFACES;
	for (b = r_bands ; b<r_bands + MAX_BANDS ; b++)
	{
		Mem_Free (b->edges);
		Mem_Free (b->edgestarts);
		Mem_Free (b->surfmem);
		b->edges = NULL;
		b->edgestarts = NULL;
		b->surfmem = NULL;
		b->maxedges = b->maxsurfs = 0;
		b->world = NULL;
	}
}

/*
==============
R_AllocBandEdges

A band's edges and surfaces, as many as asked
==============
*/
static void R_AllocBandEdges (rband_t *b, int numedges, int numsurfs)
{
	Mem_Free (b->edges);
	Mem_Free (b->edgestarts);
	b->maxedges = numedges;
	b->edges = Mem_Calloc ((size_t)numedges, sizeof(*b->edges));
	b->edgestarts = Mem_Alloc ((size_t)numedges * sizeof(*b->edgestarts));

	Mem_Free (b->surfmem);
	b->maxsurfs = numsurfs;
	b->surfmem = Mem_Calloc ((size_t)numsurfs, sizeof(*b->surfmem));
	b->surf_max = &b->surfmem[numsurfs];
// surface 0 doesn't really exist; it's just a dummy because index 0
// is used to indicate no edge attached to surface
	b->surfaces = b->surfmem - 1;
}

/*
==============
R_AllocBandSpans
==============
*/
static void R_AllocBandSpans (rband_t *b, int numspans)
{
	Mem_Free (b->spans);
	b->maxspans = numspans;
	b->spans = Mem_Alloc ((size_t)numspans * sizeof(*b->spans));
}

/*
==============
R_AllocBandAfters
==============
*/
static void R_AllocBandAfters (rband_t *b, int numafters)
{
	Mem_Free (b->afters);
	b->maxafters = numafters;
	b->afters = Mem_Alloc ((size_t)numafters * sizeof(*b->afters));
}

/*
==============
R_BandRoom

What a band needs before it runs: arrays for the world and for the frame
buffer's size, and something of everything else
==============
*/
static void R_BandRoom (rband_t *b)
{
	model_t	*world = r_scene.worldmodel;
	int		spans;

	if (b->world != world)
	{
		Mem_Free (b->surfvisible);
		Mem_Free (b->leafkeys);
		Mem_Free (b->edgecache);
		Mem_Free (b->edgenearzi);
		b->surfvisible = Mem_Alloc ((size_t)(world->numsurfaces + 7) >> 3);
		b->leafkeys = Mem_Calloc ((size_t)world->numloadedleafs, sizeof(*b->leafkeys));
		b->edgecache = Mem_Calloc ((size_t)world->numedges, sizeof(*b->edgecache));
		b->edgenearzi = Mem_Alloc ((size_t)world->numedges * sizeof(*b->edgenearzi));
		b->world = world;
	}

	if (b->width != r_edgewidth || b->height != r_edgeheight)
	{
		Mem_Free (b->removeedges);
		Mem_Free (b->linestart);
		Mem_Free (b->columnstart);
		b->removeedges = Mem_Calloc ((size_t)r_edgeheight, sizeof(*b->removeedges));
		b->linestart = Mem_Alloc ((size_t)(r_edgeheight + 2) * sizeof(*b->linestart));
		b->columnstart = Mem_Alloc ((size_t)(r_edgewidth + 1) * sizeof(*b->columnstart));
		b->width = r_edgewidth;
		b->height = r_edgeheight;

	// a line's worth of spans more than the lines' share of a view's worth
		spans = r_edgewidth * (4 + (b->bottom - b->top) / 8);
		R_AllocBandSpans (b, spans > MINSPANS ? spans : MINSPANS);
	}

	if (!b->edges)
		R_AllocBandEdges (b, r_bandedges, r_bandsurfs);
	if (!b->bedges)
		R_GrowBandBModelClip (b);
	if (!b->afters)
		R_AllocBandAfters (b, MIN_AFTERS);
}

/*
==============
R_BandCullPlane

A plane through the view's origin and the line v of the view, for culling
what's wholly above it (above) or below it; view space's y is up and z ahead,
and a point is on line ycenter - yscale * y / z
==============
*/
static void R_BandCullPlane (mplane_t *plane, int *pindex, float v, bool above)
{
	float	t;
	int		j;

	t = (ycenter - v) / yscale;
	for (j=0 ; j<3 ; j++)
	{
		plane->normal[j] = above ? t * vpn[j] - vup[j] : vup[j] - t * vpn[j];
		if (plane->normal[j] < 0)
		{
			pindex[j] = j;
			pindex[j+3] = j+3;
		}
		else
		{
			pindex[j] = j+3;
			pindex[j+3] = j;
		}
	}
	plane->dist = DotProduct (r_origin, plane->normal);
}

/*
==============
R_MeasureBands

The lines' cost from the bands' times, each band's spread over its lines and
averaged with what the lines took before
==============
*/
static void R_MeasureBands (void)
{
	rband_t	*b;
	int		v;
	float	cost;

	if (r_linecostview.x != r_refdef.vrect.x || r_linecostview.y != r_refdef.vrect.y
		|| r_linecostview.width != r_refdef.vrect.width || r_linecostview.height != r_refdef.vrect.height)
	{
		for (v=r_refdef.vrect.y ; v<r_refdef.vrectbottom ; v++)
			r_linecost[v] = 1;
		r_linecostview = r_refdef.vrect;
		return;
	}

	for (b = r_bands ; b<r_bands + r_numbands ; b++)
	{
		if (b->bottom <= b->top)
			continue;
		cost = (float)(b->time / (b->bottom - b->top));
		for (v=b->top ; v<b->bottom ; v++)
			r_linecost[v] = 0.5f * r_linecost[v] + 0.5f * cost;
	}
}

/*
==============
R_LayOutBands

The view's lines shared out among the bands, as much of their cost to each;
and each band's cull planes, a line or two out, so nothing on the band's lines
is culled
==============
*/
static void R_LayOutBands (int numbands)
{
	rband_t	*b;
	int		i, lines, line, last;
	double	total, cost, goal;

	lines = r_refdef.vrect.height;
	if (numbands > lines / MIN_BAND_LINES)
		numbands = lines / MIN_BAND_LINES;
	if (numbands < 1)
		numbands = 1;
	if (numbands > MAX_BANDS)
		numbands = MAX_BANDS;
	r_numbands = numbands;

	total = 0;
	for (line=r_refdef.vrect.y ; line<r_refdef.vrectbottom ; line++)
		total += r_linecost[line];

	cost = 0;
	line = r_refdef.vrect.y;
	for (i=0 ; i<numbands ; i++)
	{
		b = &r_bands[i];
		b->top = line;

	// to its share of the cost, but not so far the bands after can't have
	// their least
		last = r_refdef.vrectbottom - (numbands - 1 - i) * MIN_BAND_LINES;
		goal = total * (i + 1) / numbands;
		while (line < last && (line - b->top < MIN_BAND_LINES || cost < goal))
			cost += r_linecost[line++];
		b->bottom = line;

		b->cullflags = 0;
		if (b->top > r_refdef.vrect.y)
		{
			b->cullflags |= 16;
			R_BandCullPlane (&b->cullplanes[0], b->cullindexes[0], (float)(b->top - 2), true);
		}
		if (b->bottom < r_refdef.vrectbottom)
		{
			b->cullflags |= 32;
			R_BandCullPlane (&b->cullplanes[1], b->cullindexes[1], (float)(b->bottom + 1), false);
		}
	}
}

/*
==============
R_BandWorldView

A band's view back to the world's, after a brush entity's
==============
*/
void R_BandWorldView (rband_t *b)
{
	VectorCopy (base_vpn, b->vpn);
	VectorCopy (base_vup, b->vup);
	VectorCopy (base_vright, b->vright);
	VectorCopy (r_origin, b->modelorg);
	memcpy (b->clipplanes, view_clipplanes, sizeof(b->clipplanes));
}

/*
==============
R_RunBand

A band's walk and scan: on a worker thread, so nothing that isn't the band's
is changed, and nothing is allocated or printed
==============
*/
static void R_RunBand (void *ctx, int index)
{
	rband_t	*b = &r_bands[((int *)ctx)[index]];
	double	start = Sys_DoubleTime ();

	R_BeginEdgeFrame (b);
	R_RenderWorld (b);
	R_DrawBEntities (b);

// no room for the spans of edges and surfaces that aren't all there
	if (!b->outofedges && !b->outofsurfaces)
		R_ScanEdges (b);

	b->time = Sys_DoubleTime () - start;
}

/*
==============
R_NeedsMoreRoom

Whether a band that has run must run again, given more room for what it ran
out of; a brush entity's clipping that can't have more is drawn as it is
==============
*/
static bool R_NeedsMoreRoom (rband_t *b)
{
	bool	again = false;

	if (b->outofedges || b->outofsurfaces)
	{
		R_AllocBandEdges (b, b->outofedges ? b->maxedges * 2 : b->maxedges,
			b->outofsurfaces ? b->maxsurfs * 2 : b->maxsurfs);
		again = true;
	}
	if (b->outofspans)
	{
		R_AllocBandSpans (b, b->maxspans * 2);
		again = true;
	}
	if (b->outofafters)
	{
		R_AllocBandAfters (b, b->maxafters * 2);
		again = true;
	}
	if (b->outofbmodel && R_GrowBandBModelClip (b))
		again = true;
	return again;
}

/*
==============
R_RunBands

The view's bands, laid out for numbands, run on the worker threads until each
has had the room it needs
==============
*/
void R_RunBands (int numbands)
{
	int		i;

	R_MeasureBands ();
	R_LayOutBands (numbands);
	for (i=0 ; i<r_numbands ; i++)
	{
		R_BandRoom (&r_bands[i]);
		r_pending[i] = i;
	}
	r_numpending = r_numbands;

	while (r_numpending)
	{
		Sys_Parallel (r_numpending, R_RunBand, r_pending);

		for (i = 0, r_numpending = 0 ; i<r_numbands ; i++)
			if (R_NeedsMoreRoom (&r_bands[i]))
				r_pending[r_numpending++] = i;
	}

	for (i=0 ; i<r_numbands ; i++)
	{
		c_faceclip += r_bands[i].faceclip;
		r_polycount += r_bands[i].polycount;
	}
}
