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
// r_efrag.c

#include "r_local.h"
#include "r_local.h"

mnode_t	*r_pefragtopnode;


//===========================================================================

static arena_t	r_efrag_arena;		// efrags for the current level
static efrag_t	*r_free_efrags;

/*
================
R_ClearEfrags
================
*/
void R_ClearEfrags (void)
{
	if (!r_efrag_arena.name)
		Arena_Init (&r_efrag_arena, "efrags");
	Arena_Reset (&r_efrag_arena);
	r_free_efrags = NULL;
}

/*
================
R_AllocEfrag

Efrags are handed out in batches and never returned before R_ClearEfrags.
================
*/
static efrag_t *R_AllocEfrag (void)
{
	efrag_t	*ef;
	int		i;

	if (!r_free_efrags)
	{
		r_free_efrags = Arena_Alloc (&r_efrag_arena, 256 * sizeof(efrag_t));
		for (i = 0 ; i < 255 ; i++)
			r_free_efrags[i].entnext = &r_free_efrags[i+1];
	}
	ef = r_free_efrags;
	r_free_efrags = ef->entnext;
	return ef;
}

/*
===============================================================================

					ENTITY FRAGMENT FUNCTIONS

===============================================================================
*/

static efrag_t		**lastlink;

vec3_t		r_emins, r_emaxs;

static entity_t	*r_addent;

/*
===================
R_MarkEfragNodes

A leaf that has static entities now, and the nodes above it, for
R_StoreStaticEntities to find
===================
*/
void R_MarkEfragNodes (mleaf_t *leaf)
{
	mnode_t	*node;

	for (node = (mnode_t *)leaf ; node && !node->efragged ; node = node->parent)
		node->efragged = true;
}

/*
===================
R_SplitEntityOnNode
===================
*/
static void R_SplitEntityOnNode (mnode_t *node)
{
	efrag_t		*ef;
	mplane_t	*splitplane;
	mleaf_t		*leaf;
	int			sides;
	
	if (node->contents == CONTENTS_SOLID)
	{
		return;
	}
	
// add an efrag if the node is a leaf

	if ( node->contents < 0)
	{
		if (!r_pefragtopnode)
			r_pefragtopnode = node;

		leaf = (mleaf_t *)node;

// grab an efrag off the free list
		ef = R_AllocEfrag ();

		ef->entity = r_addent;
		
// add the entity link	
		*lastlink = ef;
		lastlink = &ef->entnext;
		ef->entnext = NULL;
		
// set the leaf links
		ef->leaf = leaf;
		ef->leafnext = leaf->efrags;
		leaf->efrags = ef;
		R_MarkEfragNodes (leaf);
			
		return;
	}
	
// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(r_emins, r_emaxs, splitplane);
	
	if (sides == 3)
	{
	// split on this plane
	// if this is the first splitter of this bmodel, remember it
		if (!r_pefragtopnode)
			r_pefragtopnode = node;
	}
	
// recurse down the contacted sides
	if (sides & 1)
		R_SplitEntityOnNode (node->children[0]);
		
	if (sides & 2)
		R_SplitEntityOnNode (node->children[1]);
}


/*
===================
R_SplitEntityOnNode2
===================
*/
void R_SplitEntityOnNode2 (mnode_t *node)
{
	mplane_t	*splitplane;
	int			sides;

	if (node->visframe != r_visframecount)
		return;
	
	if (node->contents < 0)
	{
		if (node->contents != CONTENTS_SOLID)
			r_pefragtopnode = node; // we've reached a non-solid leaf, so it's
									//  visible and not BSP clipped
		return;
	}
	
	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(r_emins, r_emaxs, splitplane);
	
	if (sides == 3)
	{
	// remember first splitter
		r_pefragtopnode = node;
		return;
	}
	
// not split yet; recurse down the contacted side
	if (sides & 1)
		R_SplitEntityOnNode2 (node->children[0]);
	else
		R_SplitEntityOnNode2 (node->children[1]);
}


/*
===========
R_AddEfrags
===========
*/
void R_AddEfrags (entity_t *ent)
{
	model_t		*entmodel;
	int			i;
		
	if (!ent->model)
		return;

	if (ent == &r_worldentity)
		return;		// never add the world

	r_addent = ent;
			
	lastlink = &ent->efrag;
	r_pefragtopnode = NULL;
	
	entmodel = ent->model;

	for (i=0 ; i<3 ; i++)
	{
		r_emins[i] = ent->origin[i] + entmodel->mins[i];
		r_emaxs[i] = ent->origin[i] + entmodel->maxs[i];
	}

	R_SplitEntityOnNode (r_scene.worldmodel->nodes);

	ent->topnode = r_pefragtopnode;
}


/*
================
R_StoreEfrags

// FIXME: a lot of this goes away with edge-based
================
*/
void R_StoreEfrags (efrag_t **ppefrag)
{
	entity_t	*pent;
	model_t		*clmodel;
	efrag_t		*pefrag;


	while ((pefrag = *ppefrag) != NULL)
	{
		pent = pefrag->entity;
		clmodel = pent->model;

		switch (clmodel->type)
		{
		case mod_alias:
		case mod_brush:
		case mod_sprite:
			pent = pefrag->entity;

			if ((pent->visframe != r_framecount) &&
				((*r_scene.numvisedicts) < r_scene.maxvisedicts))
			{
				r_scene.visedicts[(*r_scene.numvisedicts)++] = *pent;

			// mark that we've recorded this entity for this frame
				pent->visframe = r_framecount;
			}

			ppefrag = &pefrag->leafnext;
			break;

		default:	
			Sys_Error ("R_StoreEfrags: Bad entity type %d\n", clmodel->type);
		}
	}
}


/*
================
R_StoreStaticEntitiesNode

The walk of R_RecursiveWorldNode over the view, where there are static
entities: stored in the order it meets them
================
*/
static void R_StoreStaticEntitiesNode (mnode_t *node, int clipflags)
{
	int			i, side, *pindex;
	vec3_t		acceptpt, rejectpt;
	mplane_t	*plane;
	double		d, dot;

	if (!node->efragged)
		return;

	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;

// cull the clipping planes if not trivial accept
	if (clipflags)
	{
		for (i=0 ; i<4 ; i++)
		{
			if (! (clipflags & (1<<i)) )
				continue;	// don't need to clip against it

			pindex = pfrustum_indexes[i];

			rejectpt[0] = (float)node->minmaxs[pindex[0]];
			rejectpt[1] = (float)node->minmaxs[pindex[1]];
			rejectpt[2] = (float)node->minmaxs[pindex[2]];
			
			d = DotProduct (rejectpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d <= 0)
				return;

			acceptpt[0] = (float)node->minmaxs[pindex[3+0]];
			acceptpt[1] = (float)node->minmaxs[pindex[3+1]];
			acceptpt[2] = (float)node->minmaxs[pindex[3+2]];

			d = DotProduct (acceptpt, view_clipplanes[i].normal);
			d -= view_clipplanes[i].dist;

			if (d >= 0)
				clipflags &= ~(1<<i);	// node is entirely on screen
		}
	}

	if (node->contents < 0)
	{
		R_StoreEfrags (&((mleaf_t *)node)->efrags);
		return;
	}

// front side first
	plane = node->plane;

	switch (plane->type)
	{
	case PLANE_X:
		dot = r_origin[0] - plane->dist;
		break;
	case PLANE_Y:
		dot = r_origin[1] - plane->dist;
		break;
	case PLANE_Z:
		dot = r_origin[2] - plane->dist;
		break;
	default:
		dot = DotProduct (r_origin, plane->normal) - plane->dist;
		break;
	}

	side = dot >= 0 ? 0 : 1;
	R_StoreStaticEntitiesNode (node->children[side], clipflags);
	R_StoreStaticEntitiesNode (node->children[!side], clipflags);
}

/*
================
R_StoreStaticEntities

The static entities in the leaves of the view, added to the visible entities
in the order the world's walk meets them, before the bands walk it
================
*/
void R_StoreStaticEntities (void)
{
	R_StoreStaticEntitiesNode (r_scene.worldmodel->nodes, 15);
}
