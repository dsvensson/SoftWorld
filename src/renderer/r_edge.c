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

// a band's surfaces are generated in front to back order by the bsp;
// surfaces[1] is the background, and is used as the active surface stack

extern	int	screenwidth;

static void R_LeadingEdge (rband_t *b, edge_t *edge);
static void R_TrailingEdge (rband_t *b, surf_t *surf, edge_t *edge);


//=============================================================================

/*
==============
R_BeginEdgeFrame

A band's edges, surfaces, spans and walk, empty
==============
*/
void R_BeginEdgeFrame (rband_t *b)
{
	b->edge_p = b->edges;
	b->edge_max = &b->edges[b->maxedges];
	b->pass++;

	b->surface_p = &b->surfaces[2];	// background is surface 1,
								//  surface 0 is a dummy
	b->surfaces[1].spans = NULL;	// no background spans yet
	b->surfaces[1].flags = SURF_DRAWBACKGROUND;

// put the background behind everything in the world
	b->surfaces[1].key = 0x7FFFFFFF;
	b->currentkey = 0;

	memset (b->removeedges + b->top, 0, (size_t)(b->bottom - b->top) * sizeof(*b->removeedges));
	memset (b->surfvisible, 0, (size_t)(r_scene.worldmodel->numsurfaces + 7) >> 3);
	b->span_p = b->spans;
	b->numafters = 0;
	b->outofedges = b->outofsurfaces = b->outofspans = b->outofbmodel = b->outofafters = false;
	b->faceclip = b->polycount = 0;
	R_BandWorldView (b);
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
R_EdgeLine

The line of the band an edge is new on: the one it starts on, or the band's
first for one that starts above it
==============
*/
static inline int R_EdgeLine (const rband_t *b, uint32_t e)
{
	int		v = (int)(b->edgestarts[e] >> 1);

	return v > b->top ? v : b->top;
}

/*
==============
R_EdgeBefore

Whether edge e1 goes before edge e2 on the band's line both are new on: on u,
and at the same u leading edges before trailing ones, the leading edges the
latest emitted first and the trailing edges the earliest. That is the order
id's sorted insertion as each was emitted gave them, which the spans depend
on.

On the band's first line, the edges that started above it are where the scan
would have them, had it run from the view's top: after the edges new there at
the same u, as those were inserted before them; among themselves on the u of
the line before, so the steeper step first; and the same step, the later to
start first, as it was inserted before the other; or, starting on the same
line, as they were sorted there.
==============
*/
static inline bool R_EdgeBefore (const rband_t *b, uint32_t e1, uint32_t e2)
{
	uint32_t	trailing;
	int			v1, v2;

	if (b->edges[e1].u != b->edges[e2].u)
		return b->edges[e1].u < b->edges[e2].u;

	v1 = (int)(b->edgestarts[e1] >> 1);
	v2 = (int)(b->edgestarts[e2] >> 1);
	if ((v1 < b->top) != (v2 < b->top))
		return v1 >= b->top;
	if (v1 < b->top)
	{
		if (b->edges[e1].u_step != b->edges[e2].u_step)
			return b->edges[e1].u_step > b->edges[e2].u_step;
		if (v1 != v2)
			return v1 > v2;
	}

	trailing = b->edgestarts[e1] & 1;
	if (trailing != (b->edgestarts[e2] & 1))
		return !trailing;
	return trailing ? e1 < e2 : e1 > e2;
}

/*
==============
R_SortNewEdges

The band's edges by the line each is new on (R_EdgeLine), and there in the
order R_EdgeBefore gives; linestart[v] is where line v's are. A counting sort on
the pixel column and then one on the line leave only edges in the same column
of a line out of order, which an insertion sort puts right. The edges none of
the band's lines has are left out.
==============
*/
static void R_SortNewEdges (rband_t *b)
{
	int			i, j, n, v, ncolumns, count, total;
	uint32_t	e;

	n = (int)(b->edge_p - b->edges);
	if (n > b->maxsortededges)
	{
		Mem_Free (b->sortededges);
		Mem_Free (b->sortbuffer);
		b->maxsortededges = b->maxedges;
		b->sortededges = Mem_Alloc ((size_t)b->maxsortededges * sizeof(*b->sortededges));
		b->sortbuffer = Mem_Alloc ((size_t)b->maxsortededges * sizeof(*b->sortbuffer));
	}

// count the edges of each column, and of each line one place on (for the
// scatter below to leave linestart[v] at the start of line v)
	ncolumns = r_refdef.vrect.width + 1;
	memset (b->columnstart, 0, (size_t)ncolumns * sizeof(*b->columnstart));
	memset (b->linestart, 0, (size_t)(b->height + 2) * sizeof(*b->linestart));
	for (i=0 ; i<n ; i++)
	{
		if (b->edgestarts[i] == EDGE_OUTSIDE)
			continue;
		b->columnstart[R_EdgeColumn (&b->edges[i], ncolumns)]++;
		b->linestart[R_EdgeLine (b, (uint32_t)i) + 2]++;
	}

// by column
	total = 0;
	for (i=0 ; i<ncolumns ; i++)
	{
		count = b->columnstart[i];
		b->columnstart[i] = total;
		total += count;
	}
	for (i=0 ; i<n ; i++)
		if (b->edgestarts[i] != EDGE_OUTSIDE)
			b->sortbuffer[b->columnstart[R_EdgeColumn (&b->edges[i], ncolumns)]++] = (uint32_t)i;

// then by line, keeping the columns' order
	for (v=2 ; v<b->height + 2 ; v++)
		b->linestart[v] += b->linestart[v - 1];
	for (i=0 ; i<total ; i++)
	{
		e = b->sortbuffer[i];
		b->sortededges[b->linestart[R_EdgeLine (b, e) + 1]++] = e;
	}

// and within each column of a line
	for (v=b->top ; v<b->bottom ; v++)
	{
		for (i=b->linestart[v] + 1 ; i<b->linestart[v + 1] ; i++)
		{
			e = b->sortededges[i];
			for (j=i ; j>b->linestart[v] && R_EdgeBefore (b, e, b->sortededges[j - 1]) ; j--)
				b->sortededges[j] = b->sortededges[j - 1];
			b->sortededges[j] = e;
		}
	}
}

/*
==============
R_InsertNewEdges

Adds the count edges of toadd, indices into the band's edges sorted on u, to
the edges in the linked list edgelist.  edgelist is assumed to be sorted on u,
with a sentinel at the end (actually, this is the active edge table starting
at edge_head.next).
==============
*/
static void R_InsertNewEdges (rband_t *b, const uint32_t *toadd, int count, edge_t *edgelist)
{
	edge_t	*edgestoadd;

	for ( ; count > 0 ; count--, toadd++)
	{
		edgestoadd = &b->edges[*toadd];
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
static void R_StepActiveU (rband_t *b, edge_t *pedge)
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
		if (pedge == &b->edge_aftertail)
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
		if (pedge == &b->edge_tail)
			return;
	}
}



/*
==============
R_EmitSpan

A span of surf on the line scanned, from its last_u to iu: put in front of
its spans, as the lines go down. Its pixels are counted and its first line
kept, which the drawing shares a big surface out by (D_DrawSurfaces).
==============
*/
static inline void R_EmitSpan (rband_t *b, surf_t *surf, int iu)
{
	espan_t	*span;

	if (iu <= surf->last_u)
		return;
	if (!surf->spans)
	{
		surf->pixels = 0;
		surf->vtop = b->current_iv;
	}
	span = b->span_p++;
	span->u = surf->last_u;
	span->count = iu - span->u;
	span->v = b->current_iv;
	span->pnext = surf->spans;
	surf->spans = span;
	surf->pixels += span->count;
}

/*
==============
R_CleanupSpan
==============
*/
static void R_CleanupSpan (rband_t *b)
{
	surf_t	*surf;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
	surf = b->surfaces[1].next;
	R_EmitSpan (b, surf, b->edge_tail_u_shift20);

// reset spanstate for all surfaces in the surface stack
	do
	{
		surf->spanstate = 0;
		surf = surf->next;
	} while (surf != &b->surfaces[1]);
}


/*
==============
R_TrailingEdge
==============
*/
static void R_TrailingEdge (rband_t *b, surf_t *surf, edge_t *edge)
{
	int				iu;

// don't generate a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we haven't seen the
// start edge yet)
	if (--surf->spanstate == 0)
	{
		if (surf->insubmodel)
			b->bmodelactive--;

		if (surf == b->surfaces[1].next)
		{
		// emit a span (current top going away)
			iu = (int)(edge->u >> 20);
			R_EmitSpan (b, surf, iu);

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
static void R_LeadingEdge (rband_t *b, edge_t *edge)
{
	surf_t			*surf, *surf2;
	int				iu;
	double			fu, newzi, testzi, newzitop, newzibottom;

	if (edge->surfs[1])
	{
	// it's adding a new surface in, so find the correct place
		surf = &b->surfaces[edge->surfs[1]];

	// don't start a span if this is an inverted span, with the end
	// edge preceding the start edge (that is, we've already seen the
	// end edge)
		if (++surf->spanstate == 1)
		{
			if (surf->insubmodel)
				b->bmodelactive++;

			surf2 = b->surfaces[1].next;

			if (surf->key < surf2->key)
				goto newtop;

		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (surf->insubmodel && (surf->key == surf2->key))
			{
			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
				newzi = surf->d_ziorigin + b->fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + b->fv*surf2->d_zistepv +
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
				newzi = surf->d_ziorigin + b->fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + b->fv*surf2->d_zistepv +
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
			R_EmitSpan (b, surf2, iu);

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
static void R_GenerateSpans (rband_t *b)
{
	edge_t			*edge;
	surf_t			*surf;

	b->bmodelactive = 0;

// clear active surfaces to just the background surface
	b->surfaces[1].next = b->surfaces[1].prev = &b->surfaces[1];
	b->surfaces[1].last_u = b->edge_head_u_shift20;

// generate spans
	for (edge=b->edge_head.next ; edge != &b->edge_tail; edge=edge->next)
	{			
		if (edge->surfs[0])
		{
		// it has a left surface, so a surface is going away for this span
			surf = &b->surfaces[edge->surfs[0]];

			R_TrailingEdge (b, surf, edge);

			if (!edge->surfs[1])
				continue;
		}

		R_LeadingEdge (b, edge);
	}

	R_CleanupSpan (b);
}



/*
==============
R_ScanEdges

Input: 
the band's edgestarts[], the line each of its edges starts on
	this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans on the band's lines, or
the band ran out of room for them (outofspans)
==============
*/
void R_ScanEdges (rband_t *b)
{
	int		iv;

	R_SortNewEdges (b);

// room for a line's spans is left at the end: a line has at most one for each
// pixel
	b->max_span_p = &b->spans[b->maxspans - r_refdef.vrect.width];

	b->span_p = b->spans;

// clear active edges to just the background edges around the whole screen
// FIXME: most of this only needs to be set up once
	b->edge_head.u = (int64_t)r_refdef.vrect.x << 20;
	b->edge_head_u_shift20 = (int)(b->edge_head.u >> 20);
	b->edge_head.u_step = 0;
	b->edge_head.prev = NULL;
	b->edge_head.next = &b->edge_tail;
	b->edge_head.surfs[0] = 0;
	b->edge_head.surfs[1] = 1;
	
	b->edge_tail.u = ((int64_t)r_refdef.vrectright << 20) + 0xFFFFF;
	b->edge_tail_u_shift20 = (int)(b->edge_tail.u >> 20);
	b->edge_tail.u_step = 0;
	b->edge_tail.prev = &b->edge_head;
	b->edge_tail.next = &b->edge_aftertail;
	b->edge_tail.surfs[0] = 1;
	b->edge_tail.surfs[1] = 0;
	
	b->edge_aftertail.u = -1;		// force a move
	b->edge_aftertail.u_step = 0;
	b->edge_aftertail.next = &b->edge_sentinel;
	b->edge_aftertail.prev = &b->edge_tail;

// FIXME: do we need this now that we clamp x in r_draw.c?
	b->edge_sentinel.u = INT64_MAX;		// make sure nothing sorts past this
	b->edge_sentinel.prev = &b->edge_aftertail;

//	
// process the band's scan lines
//
	for (iv=b->top ; iv<b->bottom ; iv++)
	{
		if (b->span_p > b->max_span_p)
		{
			b->outofspans = true;
			return;
		}

		b->current_iv = iv;
		b->fv = (float)iv;

	// mark that the head (background start) span is pre-included
		b->surfaces[1].spanstate = 1;

		if (b->linestart[iv] < b->linestart[iv + 1])
			R_InsertNewEdges (b, &b->sortededges[b->linestart[iv]], b->linestart[iv + 1] - b->linestart[iv],
				b->edge_head.next);

		R_GenerateSpans (b);

		if (iv == b->bottom - 1)
			break;		// no need to step or remove on the last line

		if (b->removeedges[iv])
			R_RemoveEdges (b->removeedges[iv]);

		if (b->edge_head.next != &b->edge_tail)
			R_StepActiveU (b, b->edge_head.next);
	}
}
