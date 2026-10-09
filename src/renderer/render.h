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
	int						oldframe;		// the frame it is turning from (r_lerpframes)
	float					backlerp;		// how much of oldframe is drawn: 0 none, 1 all
	const byte				*translate;		// player colors: a palette index remap, NULL for none
	const pixel_t			*palette;		// and as the palette in them, for RGB lighting
	int						skinnum;		// for Alias models
	byte					alpha;			// FTE: 0 and 255 are opaque, else alpha * 254

	byte					*skin;			// player skin (320x200), NULL for the model's own

	float					syncbase;

	struct efrag_s			*efrag;			// linked list of efrags (FIXME)
	int						visframe;		// last frame this entity was
											// found in an active leaf
											// only used for static objects
											
	int						dlightframe;	// dynamic lighting
	int						dlightbits;
	struct p_trailstate_s	*emitstate;		// the client's: a static entity's emitter (src/particles)
	
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
	float		viewmodel_fov_x;	// the gun's, with the same widening as fov_x
	
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
	PROF_DRAW,			// the spans of surfaces drawn, in PROF_SPANS
	PROF_SURFCACHE,		// lighting surfaces into the surface cache
	PROF_MODELS,		// alias models and sprites
	PROF_FOG,			// what is drawn before translucency, fogged
	PROF_VIEWMODEL,
	PROF_PARTICLES,
	PROF_WARP,			// the underwater warp
	PROF_2D,			// status bar, console, menus
	PROF_PRESENT,		// the frame to the screen
	PROF_COUNT
} prof_t;

double	R_ProfStart (void);						// 0 unless profiling
void	R_ProfEnd (prof_t stage, double start);

// and what the surface cache did
typedef enum
{
	PROFN_SURFACES,		// surfaces lit into the surface cache
	PROFN_TEXELS,		// their texels
	PROFN_DLIT,			// the surfaces lit because a dynamic light touches them
	PROFN_HUD,			// frames whose 2D changed and was drawn
	PROFN_BATCHES,		// batches of surfaces drawn (D_DrawSurfaces)
	PROFN_COUNT
} profn_t;

void	R_ProfCount (profn_t what, int n);
// palette.lmp and colormap.lmp, before any drawing
void R_InitPalette (const byte *palette, const byte *colormap);
pixel_t R_ColorPixel (int r, int g, int b);		// an sRGB color as a pixel (vid.h)
void R_InitTextures (void);
void R_RenderView (void);		// must set r_refdef first
void R_ViewChanged (vrect_t *vrect, float aspect);
								// called whenever r_refdef or vid change
void R_InitSky (struct texture_s *mt);	// the world's sky texture, at R_NewMap

void R_AddEfrags (entity_t *ent);

void R_NewMap (void);


void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count);
// id's trail types, a particle every 3 units; *carry (NULL for none) keeps
// the spacing even across the stretches of one entity's trail
void R_RocketTrail (const vec3_t start, const vec3_t end, int type, float *carry);

void R_BlobExplosion (vec3_t org);
void R_ParticleExplosion (vec3_t org);
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength);	// NetQuake's, in a color range
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
void D_AllocCache (int size);	// the cache, size bytes, flushed
void D_GrowCache (void);		// after a frame it ran out in: twice the room, up to a limit

// allocates the z-buffer and surface cache for a width x height view buffer
// allocates everything that depends on the size of the frame; scale is
// render pixels per pixel of the 320x200 layout
void R_SetRenderSize (int width, int height, int scale);

// a ring lying flat on the floor, part of it lit, turning: where an item is
// missing and how soon it is back (qualia's spawn rings, cl_items.c)
typedef struct
{
	vec3_t	centre;
	float	radius;
	float	fill;			// how much of it is lit, 0 .. 1
	float	phase;			// radians round from +x where the lit part starts
	float	color[3];		// 1.0 is white; more is brighter
} r_ring_t;

// scripted particles (src/particles), as QuakeSpasm-Spiked's draw them: a
// frame's, in batches each of one look, the batches in the order they blend
// (darkening, then blending, then adding), drawn after id's particles in each
// view, depth tested and not depth written
typedef enum
{
	RPT_SPRITE,			// a square facing the view, turned by angle
	RPT_SPARK,			// a line from org back along vel / 10, fading
	RPT_TSPARK,			// a quad stretched along vel, facing the view
	RPT_FAN,			// a triangle back along vel
	RPT_UDECAL,			// a square lying flat
	RPT_BEAM,			// pairs of parts: a quad between each pair
	RPT_DECAL			// triangles of verts, lying on surfaces
} r_parttype_t;

typedef enum	// GL's blend factors, source and destination
{
	RPB_BLEND,			// alpha, 1 - alpha
	RPB_BLENDCOLOR,		// color, 1 - color
	RPB_ADDA,			// alpha, 1
	RPB_ADDC,			// color, 1
	RPB_SUBTRACT,		// alpha, 1 - color
	RPB_INVMODA,		// 0, 1 - alpha
	RPB_INVMODC,		// 0, 1 - color
	RPB_PREMUL			// 1, 1 - alpha
} r_partblend_t;

typedef struct
{
	vec3_t	org;
	vec3_t	vel;		// a beam end's: the beam's direction
	float	scale;		// its size; a beam's half width
	float	angle;		// radians
	float	rgba[4];	// sRGB, 0 to 1; alpha past 1 is 1
	float	st[4];		// s1 t1 s2 t2 of its image; a beam end's s, then t1 at 1, t2 at 3
} r_part_t;

typedef struct
{
	vec3_t	xyz;
	float	st[2];
	float	rgba[4];
} r_partvert_t;

typedef struct
{
	r_parttype_t	type;
	r_partblend_t	blend;
	int		premul;		// rgba as is (0), its color times its alpha (1), and that adding (2)
	int		image;		// R_ParticleImage's
	float	scalefactor, invscalefactor;	// sprites' and fans': their size with distance
	float	stretch, minstretch;			// textured sparks': their length
	int		first, count;	// parts, or verts of RPT_DECAL
} r_partbatch_t;

typedef struct r_partscene_s
{
	const r_partbatch_t	*batches;
	int					numbatches;
	const r_part_t		*parts;
	const r_partvert_t	*verts;
} r_partscene_t;

// the images particles take when a script's file isn't there, as
// QuakeSpasm-Spiked makes them
enum {RPI_WHITE, RPI_BEAM, RPI_FAN, RPI_BALL, RPI_FUZZY, RPI_CLASSIC, RPI_NUMFALLBACKS};

// a particle image: textures/<name> or <name>, a TGA file; else the
// fallback, *found false
int		R_ParticleImage (const char *name, int fallback, bool *found);
void	R_FlushParticleImages (void);	// all forgotten: a new game directory

// a decal's triangles on a brush model's faces (not skies or liquids): what of
// them a box size across round center leaves, its sides along normal and the
// tangents, to the callback a few at a time; facing, only faces facing mostly
// against normal (r_decal.c)
void	R_ClipDecal (struct model_s *mod, const vec3_t center, const vec3_t normal, const vec3_t tangent1,
			const vec3_t tangent2, float size, bool facing,
			void (*callback) (void *ctx, const vec3_t *points, int numtris), void *ctx);

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
	const r_ring_t	*rings;		// this frame's
	int			numrings;
	const r_partscene_t	*particles;	// scripted ones, this frame's; NULL none

	int			viewcontents;	// output: contents at the view origin after R_RenderView
	int			framecount;		// output: the view's frame, the visframe of the static entities it drew
	int			visframe;		// output: the visframe of the leafs in the view's PVS
} r_scene_t;

extern r_scene_t	r_scene;

// clears all efrags; call before a new level adds static entities
void R_ClearEfrags (void);

// draws one column of a graph (performance and network graphs)
void R_LineGraph (int x, int y, int h);
extern struct cvar_s	r_graphheight;
