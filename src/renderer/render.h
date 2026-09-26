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

#include "cvar.h"
#include "mathlib.h"
#include "protocol.h"
#include "vid.h"

// refresh.h -- public interface to refresh functions

#define	TOP_RANGE		16			// soldier uniform colors
#define	BOTTOM_RANGE	96

//=============================================================================

typedef struct
{
	int		key;				// so entities can reuse same entry
	vec3_t	origin;
	float	radius;
	float	die;				// stop lighting after this time
	float	decay;				// drop this each second
	float	minlight;			// don't add when contributing less
	float   color[4];
} dlight_t;

typedef struct
{
	int		length;
	char	map[MAX_STYLESTRING];
} lightstyle_t;

#define	MAX_DLIGHTS		32

typedef struct efrag_s
{
	struct mleaf_s		*leaf;
	struct efrag_s		*leafnext;
	struct entity_s		*entity;
	struct efrag_s		*entnext;
} efrag_t;


typedef struct entity_s
{
	int						keynum;			// for matching entities in different frames
	vec3_t					origin;
	vec3_t					angles;	
	struct model_s			*model;			// NULL = no model
	int						frame;
	const byte				*translate;		// player colors: a palette index remap, NULL for none
	int						skinnum;		// for Alias models

	byte					*skin;			// player skin (320x200), NULL for the model's own

	float					syncbase;

	struct efrag_s			*efrag;			// linked list of efrags (FIXME)
	int						visframe;		// last frame this entity was
											// found in an active leaf
											// only used for static objects
											
	int						dlightframe;	// dynamic lighting
	int						dlightbits;
	
// FIXME: could turn these into a union
	int						trivial_accept;
	struct mnode_s			*topnode;		// for bmodels, first world node
											//  that splits bmodel, or NULL if
											//  not split
} entity_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	vrect_t		vrect;				// subwindow in video for refresh
									// FIXME: not need vrect next field here?
	vrect_t		aliasvrect;			// scaled Alias version
	int			vrectright, vrectbottom;	// right & bottom screen coords
	int			aliasvrectright, aliasvrectbottom;	// scaled Alias versions
	float		vrectrightedge;			// rightmost right edge we care about,
										//  for use in edge list
	float		fvrectx, fvrecty;		// for floating-point compares
	float		fvrectx_adj, fvrecty_adj; // left and top edges, for clamping
	int64_t		vrect_x_adj_shift20;	// (vrect.x + 0.5 - epsilon) << 20
	int64_t		vrectright_adj_shift20;	// (vrectright + 0.5 - epsilon) << 20
	float		fvrectright_adj, fvrectbottom_adj;
										// right and bottom edges, for clamping
	float		fvrectright;			// rightmost edge, for Alias clamping
	float		fvrectbottom;			// bottommost edge, for Alias clamping
	float		horizontalFieldOfView;	// at Z = 1.0, this many X is visible 
										// 2.0 = 90 degrees
	float		xOrigin;			// should probably allways be 0.5
	float		yOrigin;			// between be around 0.3 to 0.5

	vec3_t		vieworg;
	vec3_t		viewangles;

	float		fov_x, fov_y;
	
	int			ambientlight;
} refdef_t;


//
// refresh
//


extern	refdef_t	r_refdef;
extern vec3_t	r_origin, vpn, vright, vup;

extern	struct texture_s	*r_notexture_mip;

extern	entity_t	r_worldentity;

void R_Init (void);

// r_profile 1: time per stage of the frame, printed by r_profile_show
typedef enum
{
	PROF_EDGES,			// the world and brush models into edges and surfaces
	PROF_SPANS,			// surfaces into pixels, surface cache included
	PROF_SURFCACHE,		// lighting surfaces into the surface cache
	PROF_MODELS,		// alias models and sprites
	PROF_VIEWMODEL,
	PROF_PARTICLES,
	PROF_WARP,			// the underwater warp
	PROF_2D,			// status bar, console, menus
	PROF_PRESENT,		// the frame to the screen
	PROF_COUNT
} prof_t;

double	R_ProfStart (void);						// 0 unless profiling
void	R_ProfEnd (prof_t stage, double start);
// palette.lmp and colormap.lmp, before any drawing
void R_InitPalette (const byte *palette, const byte *colormap);
void R_InitTextures (void);
void R_RenderView (void);		// must set r_refdef first
void R_ViewChanged (vrect_t *vrect, float aspect);
								// called whenever r_refdef or vid change
void R_InitSky (struct texture_s *mt);	// called at level load

void R_AddEfrags (entity_t *ent);

void R_NewMap (void);


void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count);
void R_RocketTrail (vec3_t start, vec3_t end, int type);

void R_BlobExplosion (vec3_t org);
void R_ParticleExplosion (vec3_t org);
void R_LavaSplash (vec3_t org);
void R_TeleportSplash (vec3_t org);

void R_PushDlights (void);
void R_InitParticles (void);
void R_ClearParticles (void);
void R_DrawParticles (void);


//
// surface cache related
//
extern bool	r_cache_thrash;	// set if thrashing the surface cache

int	D_SurfaceCacheForRes (int width, int height);
void D_FlushCaches (void);
void D_InitCaches (void *buffer, int size);

// allocates the z-buffer and surface cache for a width x height view buffer
// allocates everything that depends on the size of the frame; scale is
// render pixels per pixel of the 320x200 layout
void R_SetRenderSize (int width, int height, int scale);

//
// what the client hands the renderer: set up once, updated as the game runs
//
typedef struct
{
	double		time;			// client time, drives animation
	float		frametime;		// seconds since the previous frame
	struct model_s	*worldmodel;

	entity_t	*visedicts;		// entities to draw this frame; static entities
	int			*numvisedicts;	// visible this frame are appended by the renderer
	int			maxvisedicts;
	dlight_t	*dlights;		// MAX_DLIGHTS
	lightstyle_t	*lightstyles;	// MAX_LIGHTSTYLES
	entity_t	*viewent;		// the weapon model
	bool		drawviewmodel;	// false when the player is invisible, dead or observing

	int			viewcontents;	// output: contents at the view origin after R_RenderView
} r_scene_t;

extern r_scene_t	r_scene;

// clears all efrags; call before a new level adds static entities
void R_ClearEfrags (void);

// draws one column of a graph (performance and network graphs)
void R_LineGraph (int x, int y, int h);
extern struct cvar_s	r_graphheight;
