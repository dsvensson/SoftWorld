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

#pragma once
// cmodel.h -- the collision model of the current map: clipping hulls, leafs and
// visibility. The server uses it for physics and visibility, the client for
// prediction. Both share the one loaded map.

#include "bspfile.h"
#include "mathlib.h"
#include "q_types.h"

typedef struct
{
	int			planenum;
	int			children[2];	// negative numbers are contents
} mclipnode_t;

typedef struct
{
	mclipnode_t	*clipnodes;
	mplane_t	*planes;
	int			firstclipnode;
	int			lastclipnode;
	vec3_t		clip_mins;
	vec3_t		clip_maxs;
} hull_t;

typedef struct
{
	vec3_t	normal;
	float	dist;
} plane_t;

typedef struct
{
	bool	allsolid;	// if true, plane is not valid
	bool	startsolid;	// if true, the initial point was in a solid area
	bool	inopen, inwater;
	float	fraction;	// time completed, 1.0 = didn't hit anything
	vec3_t	endpos;		// final position
	plane_t	plane;		// surface normal at impact
	union
	{
		int				entnum;	// player movement: the physent that was hit
		struct edict_s	*ent;	// server: the entity that was hit
	};
} trace_t;

// the world or one of its inline brush models ("*1", "*2", ...)
typedef struct cmodel_s
{
	vec3_t	mins, maxs;
	hull_t	hulls[MAX_MAP_HULLS];
} cmodel_t;

typedef struct cleaf_s cleaf_t;

//
// loading
//

// makes name the current map, unless it already is, and returns its world model;
// NULL if the file can't be found. Everything returned by the CM_ functions stays
// valid until a different map is loaded.
cmodel_t	*CM_LoadMap (const char *name, unsigned *checksum, unsigned *checksum2);

// "*1", "*2", ... of the current map, NULL if there is no such model
cmodel_t	*CM_InlineModel (const char *name);

// including the world
int			CM_NumInlineModels (void);

char		*CM_EntityString (void);

//
// clipping
//

int			CM_HullPointContents (const hull_t *hull, int num, const vec3_t p);

// traces from p1 to p2 through hull, starting at node num; p1f and p2f are the
// fractions of the whole move at p1 and p2. Returns false once something was hit.
bool		CM_RecursiveHullCheck (const hull_t *hull, int num, float p1f, float p2f,
				const vec3_t p1, const vec3_t p2, trace_t *trace);

// a hull for an axial box, valid until the next call
hull_t		*CM_HullForBox (const vec3_t mins, const vec3_t maxs);

//
// leafs and visibility
//

const cleaf_t	*CM_PointInLeaf (const vec3_t p);

// 0 is the solid leaf outside the map; visibility bit n is for leaf n + 1
int			CM_Leafnum (const cleaf_t *leaf);

// NUM_AMBIENTS levels
const byte	*CM_LeafAmbientLevels (const cleaf_t *leaf);

// leafs with visibility information, not counting leaf 0
int			CM_NumVisLeafs (void);

// the leafs visible from leaf leafnum, valid until the next call
byte		*CM_LeafPVS (int leafnum);

// the leafs visible from anywhere within 8 units of org, valid until the next call
byte		*CM_FatPVS (const vec3_t org);

// stores the visibility bit numbers of up to maxleafs leafs the box touches;
// returns how many were stored
int			CM_FindTouchedLeafs (const vec3_t mins, const vec3_t maxs, int *leafs, int maxleafs);
