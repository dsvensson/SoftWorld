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
// r_edge.c

#include "r_local.h"
#include "r_local.h"



edge_t	*r_edges, *edge_p, *edge_max;

surf_t	*surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack

// of each edge of the frame, by its index in r_edges: the scan line it starts
// on times two, plus one for a trailing edge (R_EmitEdge)
uint32_t	*r_edgestarts;
edge_t	**removeedges;

// the frame's edges sorted for the scan (R_SortNewEdges), as indices into
// r_edges; line v's from r_linestart[v] to r_linestart[v + 1]
static uint32_t	*r_sortededges, *r_sortbuffer;
static int		r_maxsortededges;
static int		*r_linestart;		// r_edgelines + 2
static int		*r_columnstart;		// the view's width + 1
static int		r_edgelines;
static espan_t	*basespans;		// room for r_maxspans
static int		r_maxspans;

static espan_t	*span_p, *max_span_p;

int		r_currentkey;

extern	int	screenwidth;

static int	current_iv;

static int	edge_head_u_shift20, edge_tail_u_shift20;	// in whole pixels

static void (*pdrawfunc)(void);

static edge_t	edge_head;
static edge_t	edge_tail;
static edge_t	edge_aftertail;
static edge_t	edge_sentinel;

static float	fv;

static void R_GenerateSpans (void);
static void R_GenerateSpansBackward (void);

static void R_LeadingEdge (edge_t *edge);
static void R_LeadingEdgeBackwards (edge_t *edge);
static void R_TrailingEdge (surf_t *surf, edge_t *edge);


//=============================================================================


/*
==============
R_DrawCulledPolys
==============
*/
static void R_DrawCulledPolys (void)
{
	surf_t			*s;
	msurface_t		*pface;

	currententity = &r_worldentity;

	if (r_worldpolysbacktofront)
	{
		for (s=surface_p-1 ; s>&surfaces[1] ; s--)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
}


/*
==============
R_SetEdgeSize

The edge lists of each scan line, and room for the spans of a frame; the
spans are drawn and the room reused when they run out
==============
*/
void R_SetEdgeSize (int width, int height)
{
	Mem_Free (removeedges);
	Mem_Free (r_linestart);
	Mem_Free (r_columnstart);
	Mem_Free (basespans);
	removeedges = Mem_Calloc ((size_t)height, sizeof(*removeedges));
	r_edgelines = height;
	r_linestart = Mem_Alloc ((size_t)(height + 2) * sizeof(*r_linestart));
	r_columnstart = Mem_Alloc ((size_t)(width + 1) * sizeof(*r_columnstart));
	r_maxspans = width * 4 > MINSPANS ? width * 4 : MINSPANS;
	basespans = Mem_Alloc ((size_t)r_maxspans * sizeof(*basespans));
}

/*
==============
R_BeginEdgeFrame
==============
*/
void R_BeginEdgeFrame (void)
{
	int		v;

	edge_p = r_edges;
	edge_max = &r_edges[r_numallocatededges];

	surface_p = &surfaces[2];	// background is surface 1,
								//  surface 0 is a dummy
	surfaces[1].spans = NULL;	// no background spans yet
	surfaces[1].flags = SURF_DRAWBACKGROUND;

// put the background behind everything in the world
	if (r_draworder.value)
	{
		pdrawfunc = R_GenerateSpansBackward;
		surfaces[1].key = 0;
		r_currentkey = 1;
	}
	else
	{
		pdrawfunc = R_GenerateSpans;
		surfaces[1].key = 0x7FFFFFFF;
		r_currentkey = 0;
	}

// FIXME: set with memset
	for (v=r_refdef.vrect.y ; v<r_refdef.vrectbottom ; v++)
	{
		removeedges[v] = NULL;
	}
}



/*
==============
R_EdgeColumn

The pixel column an edge starts in, of the view's: edges in different columns
are in order on u
==============
*/
static inline int R_EdgeColumn (const edge_t *edge, int ncolumns)
{
	int64_t	c;

	c = (edge->u >> 20) - r_refdef.vrect.x;
	return c < 0 ? 0 : c >= ncolumns ? ncolumns - 1 : (int)c;
}

/*
==============
R_EdgeBefore

Whether edge a goes before edge b on the scan line both start on: on u, and
at the same u leading edges before trailing ones, the leading edges the
latest emitted first and the trailing edges the earliest. That is the order
id's sorted insertion as each was emitted gave them, which the spans depend
on.
==============
*/
static inline bool R_EdgeBefore (uint32_t a, uint32_t b)
{
	uint32_t	trailing;

	if (r_edges[a].u != r_edges[b].u)
		return r_edges[a].u < r_edges[b].u;
	trailing = r_edgestarts[a] & 1;
	if (trailing != (r_edgestarts[b] & 1))
		return !trailing;
	return trailing ? a < b : a > b;
}

/*
==============
R_SortNewEdges

The frame's edges by the scan line each starts on, and there in the order
R_EdgeBefore gives; r_linestart[v] is where line v's are. A counting sort on
the pixel column and then one on the line leave only edges in the same column
of a line out of order, which an insertion sort puts right.
==============
*/
static void R_SortNewEdges (void)
{
	int			i, j, n, v, ncolumns, count, total;
	uint32_t	e;

	n = (int)(edge_p - r_edges);
	if (n > r_maxsortededges)
	{
		Mem_Free (r_sortededges);
		Mem_Free (r_sortbuffer);
		r_maxsortededges = r_numallocatededges;
		r_sortededges = Mem_Alloc ((size_t)r_maxsortededges * sizeof(*r_sortededges));
		r_sortbuffer = Mem_Alloc ((size_t)r_maxsortededges * sizeof(*r_sortbuffer));
	}

// count the edges of each column, and of each line one place on (for the
// scatter below to leave r_linestart[v] at the start of line v)
	ncolumns = r_refdef.vrect.width + 1;
	memset (r_columnstart, 0, (size_t)ncolumns * sizeof(*r_columnstart));
	memset (r_linestart, 0, (size_t)(r_edgelines + 2) * sizeof(*r_linestart));
	for (i=0 ; i<n ; i++)
	{
		r_columnstart[R_EdgeColumn (&r_edges[i], ncolumns)]++;
		r_linestart[(r_edgestarts[i] >> 1) + 2]++;
	}

// by column
	total = 0;
	for (i=0 ; i<ncolumns ; i++)
	{
		count = r_columnstart[i];
		r_columnstart[i] = total;
		total += count;
	}
	for (i=0 ; i<n ; i++)
		r_sortbuffer[r_columnstart[R_EdgeColumn (&r_edges[i], ncolumns)]++] = (uint32_t)i;

// then by line, keeping the columns' order
	for (v=2 ; v<r_edgelines + 2 ; v++)
		r_linestart[v] += r_linestart[v - 1];
	for (i=0 ; i<n ; i++)
	{
		e = r_sortbuffer[i];
		r_sortededges[r_linestart[(r_edgestarts[e] >> 1) + 1]++] = e;
	}

// and within each column of a line
	for (v=r_refdef.vrect.y ; v<r_refdef.vrectbottom ; v++)
	{
		for (i=r_linestart[v] + 1 ; i<r_linestart[v + 1] ; i++)
		{
			e = r_sortededges[i];
			for (j=i ; j>r_linestart[v] && R_EdgeBefore (e, r_sortededges[j - 1]) ; j--)
				r_sortededges[j] = r_sortededges[j - 1];
			r_sortededges[j] = e;
		}
	}
}

/*
==============
R_InsertNewEdges

Adds the count edges of toadd, indices into r_edges sorted on u, to the edges
in the linked list edgelist.  edgelist is assumed to be sorted on u, with a
sentinel at the end (actually, this is the active edge table starting at
edge_head.next).
==============
*/
static void R_InsertNewEdges (const uint32_t *toadd, int count, edge_t *edgelist)
{
	edge_t	*edgestoadd;

	for ( ; count > 0 ; count--, toadd++)
	{
		edgestoadd = &r_edges[*toadd];
edgesearch:
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		goto edgesearch;

	// insert edgestoadd before edgelist
addedge:
		edgestoadd->next = edgelist;
		edgestoadd->prev = edgelist->prev;
		edgelist->prev->next = edgestoadd;
		edgelist->prev = edgestoadd;
	}
}

	


/*
==============
R_RemoveEdges
==============
*/
static void R_RemoveEdges (edge_t *pedge)
{

	do
	{
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;
	} while ((pedge = pedge->nextremove) != NULL);
}




/*
==============
R_StepActiveU
==============
*/
static void R_StepActiveU (edge_t *pedge)
{
	edge_t		*pnext_edge, *pwedge;

	while (1)
	{
nextedge:
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		goto nextedge;		
		
pushback:
		if (pedge == &edge_aftertail)
			return;
			
	// push it back to keep it sorted		
		pnext_edge = pedge->next;

	// pull the edge out of the edge list
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;

	// find out where the edge goes in the edge list
		pwedge = pedge->prev->prev;

		while (pwedge->u > pedge->u)
		{
			pwedge = pwedge->prev;
		}

	// put the edge back into the edge list
		pedge->next = pwedge->next;
		pedge->prev = pwedge;
		pedge->next->prev = pedge;
		pwedge->next = pedge;

		pedge = pnext_edge;
		if (pedge == &edge_tail)
			return;
	}
}



/*
==============
R_CleanupSpan
==============
*/
static void R_CleanupSpan (void)
{
	surf_t	*surf;
	int		iu;
	espan_t	*span;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
	surf = surfaces[1].next;
	iu = edge_tail_u_shift20;
	if (iu > surf->last_u)
	{
		span = span_p++;
		span->u = surf->last_u;
		span->count = iu - span->u;
		span->v = current_iv;
		span->pnext = surf->spans;
		surf->spans = span;
	}

// reset spanstate for all surfaces in the surface stack
	do
	{
		surf->spanstate = 0;
		surf = surf->next;
	} while (surf != &surfaces[1]);
}


/*
==============
R_LeadingEdgeBackwards
==============
*/
static void R_LeadingEdgeBackwards (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;

// it's adding a new surface in, so find the correct place
	surf = &surfaces[edge->surfs[1]];

// don't start a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we've already seen the
// end edge)
	if (++surf->spanstate == 1)
	{
		surf2 = surfaces[1].next;

		if (surf->key > surf2->key)
			goto newtop;

	// if it's two surfaces on the same plane, the one that's already
	// active is in front, so keep going unless it's a bmodel
		if (surf->insubmodel && (surf->key == surf2->key))
		{
		// must be two bmodels in the same leaf; don't care, because they'll
		// never be farthest anyway
			goto newtop;
		}

continue_search:

		do
		{
			surf2 = surf2->next;
		} while (surf->key < surf2->key);

		if (surf->key == surf2->key)
		{
		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (!surf->insubmodel)
				goto continue_search;

		// must be two bmodels in the same leaf; don't care which is really
		// in front, because they'll never be farthest anyway
		}

		goto gotposition;

newtop:
	// emit a span (obscures current top)
		iu = (int)(edge->u >> 20);

		if (iu > surf2->last_u)
		{
			span = span_p++;
			span->u = surf2->last_u;
			span->count = iu - span->u;
			span->v = current_iv;
			span->pnext = surf2->spans;
			surf2->spans = span;
		}

		// set last_u on the new span
		surf->last_u = iu;
				
gotposition:
	// insert before surf2
		surf->next = surf2;
		surf->prev = surf2->prev;
		surf2->prev->next = surf;
		surf2->prev = surf;
	}
}


/*
==============
R_TrailingEdge
==============
*/
static void R_TrailingEdge (surf_t *surf, edge_t *edge)
{
	espan_t			*span;
	int				iu;

// don't generate a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we haven't seen the
// start edge yet)
	if (--surf->spanstate == 0)
	{
		if (surf->insubmodel)
			r_bmodelactive--;

		if (surf == surfaces[1].next)
		{
		// emit a span (current top going away)
			iu = (int)(edge->u >> 20);
			if (iu > surf->last_u)
			{
				span = span_p++;
				span->u = surf->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf->spans;
				surf->spans = span;
			}

		// set last_u on the surface below
			surf->next->last_u = iu;
		}

		surf->prev->next = surf->next;
		surf->next->prev = surf->prev;
	}
}



/*
==============
R_LeadingEdge
==============
*/
static void R_LeadingEdge (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;
	double			fu, newzi, testzi, newzitop, newzibottom;

	if (edge->surfs[1])
	{
	// it's adding a new surface in, so find the correct place
		surf = &surfaces[edge->surfs[1]];

	// don't start a span if this is an inverted span, with the end
	// edge preceding the start edge (that is, we've already seen the
	// end edge)
		if (++surf->spanstate == 1)
		{
			if (surf->insubmodel)
				r_bmodelactive++;

			surf2 = surfaces[1].next;

			if (surf->key < surf2->key)
				goto newtop;

		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (surf->insubmodel && (surf->key == surf2->key))
			{
			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto newtop;
				}

				newzitop = newzi * 1.01;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto newtop;
					}
				}
			}

continue_search:

			do
			{
				surf2 = surf2->next;
			} while (surf->key > surf2->key);

			if (surf->key == surf2->key)
			{
			// if it's two surfaces on the same plane, the one that's already
			// active is in front, so keep going unless it's a bmodel
				if (!surf->insubmodel)
					goto continue_search;

			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto gotposition;
				}

				newzitop = newzi * 1.01;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto gotposition;
					}
				}

				goto continue_search;
			}

			goto gotposition;

newtop:
		// emit a span (obscures current top)
			iu = (int)(edge->u >> 20);

			if (iu > surf2->last_u)
			{
				span = span_p++;
				span->u = surf2->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf2->spans;
				surf2->spans = span;
			}

			// set last_u on the new span
			surf->last_u = iu;
				
gotposition:
		// insert before surf2
			surf->next = surf2;
			surf->prev = surf2->prev;
			surf2->prev->next = surf;
			surf2->prev = surf;
		}
	}
}


/*
==============
R_GenerateSpans
==============
*/
static void R_GenerateSpans (void)
{
	edge_t			*edge;
	surf_t			*surf;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{			
		if (edge->surfs[0])
		{
		// it has a left surface, so a surface is going away for this span
			surf = &surfaces[edge->surfs[0]];

			R_TrailingEdge (surf, edge);

			if (!edge->surfs[1])
				continue;
		}

		R_LeadingEdge (edge);
	}

	R_CleanupSpan ();
}



/*
==============
R_GenerateSpansBackward
==============
*/
static void R_GenerateSpansBackward (void)
{
	edge_t			*edge;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{			
		if (edge->surfs[0])
			R_TrailingEdge (&surfaces[edge->surfs[0]], edge);

		if (edge->surfs[1])
			R_LeadingEdgeBackwards (edge);
	}

	R_CleanupSpan ();
}


/*
==============
R_ScanEdges

Input: 
r_edgestarts[], the line each edge of r_edges starts on
	this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans
==============
*/
void R_ScanEdges (void)
{
	int		iv, bottom;
	espan_t	*basespan_p;
	surf_t	*s;

	R_SortNewEdges ();

	basespan_p = basespans;
	max_span_p = &basespan_p[r_maxspans - r_refdef.vrect.width];

	span_p = basespan_p;

// clear active edges to just the background edges around the whole screen
// FIXME: most of this only needs to be set up once
	edge_head.u = (int64_t)r_refdef.vrect.x << 20;
	edge_head_u_shift20 = (int)(edge_head.u >> 20);
	edge_head.u_step = 0;
	edge_head.prev = NULL;
	edge_head.next = &edge_tail;
	edge_head.surfs[0] = 0;
	edge_head.surfs[1] = 1;
	
	edge_tail.u = ((int64_t)r_refdef.vrectright << 20) + 0xFFFFF;
	edge_tail_u_shift20 = (int)(edge_tail.u >> 20);
	edge_tail.u_step = 0;
	edge_tail.prev = &edge_head;
	edge_tail.next = &edge_aftertail;
	edge_tail.surfs[0] = 1;
	edge_tail.surfs[1] = 0;
	
	edge_aftertail.u = -1;		// force a move
	edge_aftertail.u_step = 0;
	edge_aftertail.next = &edge_sentinel;
	edge_aftertail.prev = &edge_tail;

// FIXME: do we need this now that we clamp x in r_draw.c?
	edge_sentinel.u = INT64_MAX;		// make sure nothing sorts past this
	edge_sentinel.prev = &edge_aftertail;

//	
// process all scan lines
//
	bottom = r_refdef.vrectbottom - 1;

	for (iv=r_refdef.vrect.y ; iv<bottom ; iv++)
	{
		current_iv = iv;
		fv = (float)iv;

	// mark that the head (background start) span is pre-included
		surfaces[1].spanstate = 1;

		if (r_linestart[iv] < r_linestart[iv + 1])
			R_InsertNewEdges (&r_sortededges[r_linestart[iv]], r_linestart[iv + 1] - r_linestart[iv],
				edge_head.next);

		(*pdrawfunc) ();

	// flush the span list if we can't be sure we have enough spans left for
	// the next scan
		if (span_p > max_span_p)
		{
			if (r_drawculledpolys)
				R_DrawCulledPolys ();
			else
				D_DrawSurfaces ();

		// clear the surface span pointers
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = NULL;

			span_p = basespan_p;
		}

		if (removeedges[iv])
			R_RemoveEdges (removeedges[iv]);

		if (edge_head.next != &edge_tail)
			R_StepActiveU (edge_head.next);
	}

// do the last scan (no need to step or sort or remove on the last scan)

	current_iv = iv;
	fv = (float)iv;

// mark that the head (background start) span is pre-included
	surfaces[1].spanstate = 1;

	if (r_linestart[iv] < r_linestart[iv + 1])
		R_InsertNewEdges (&r_sortededges[r_linestart[iv]], r_linestart[iv + 1] - r_linestart[iv],
			edge_head.next);

	(*pdrawfunc) ();

// draw whatever's left in the span list
	if (r_drawculledpolys)
		R_DrawCulledPolys ();
	else
		D_DrawSurfaces ();
}


