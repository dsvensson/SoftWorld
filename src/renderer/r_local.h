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
// r_local.h -- private refresh defs


#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "args.h"
#include "arena.h"
#include "bspfile.h"
#include "cmd.h"
#include "crc.h"
#include "cvar.h"
#include "fs.h"
#include "info.h"
#include "link.h"
#include "mathlib.h"
#include "md4.h"
#include "mem.h"
#include "msg.h"
#include "print.h"
#include "protocol.h"
#include "q_endian.h"
#include "q_string.h"
#include "q_types.h"
#include "sys.h"
#include "version.h"
#include "vmarray.h"
#include "d_iface.h"
#include "draw.h"
#include "model.h"
#include "r_shared.h"
#include "render.h"
#include "simd.h"
#include "vid.h"
#include "wad.h"

#define ALIAS_BASE_SIZE_RATIO		(1.0 / 11.0)
					// normalizing factor so player model works out to about
					//  1 pixel per triangle

#define BMODEL_FULLY_CLIPPED	0x10 // value returned by R_BmodelCheckBBox ()
									 //  if bbox is trivially rejected

//===========================================================================
// viewmodel lighting

typedef struct {
	int			ambientlight;
	int			shadelight;
	float		*plightvec;
	float		color[3];		// the light's color, the brightest channel 1
} alight_t;

//===========================================================================
// clipped bmodel edges

typedef struct bedge_s
{
	mvertex_t		*v[2];
	struct bedge_s	*pnext;
} bedge_t;

typedef struct {
	float	fv[3];		// viewspace x, y
} auxvert_t;

//===========================================================================

extern cvar_t	r_draworder;
extern cvar_t	r_graphheight;
extern cvar_t	r_clearcolor;
extern cvar_t	r_waterwarp;
extern cvar_t	r_fullbright;

extern pixel_t	d_pal30[256];				// the palette
extern pixel_t	d_cm30[VID_GRADES * 256];	// the palette through each colormap row
extern byte		r_identityremap[256];		// no player colors
extern byte		d_palrgb[256][3];
extern pixel_t	d_pal30_floor[256];
extern bool		d_fullbright[256];
extern int		d_glowscale;				// as simd_litrow_rgb30 takes r_fullbright_scale
extern pixel_t	d_pal30_unlit[256];			// what light doesn't reach: particles, sprites,
extern bool		d_unlitfloors;				// liquids, sky (R_SetUnlitColors)
extern pixel_t	d_pal30_particle[256];		// particles', the fire ramp glowing too
double		R_SrgbToLinear (double c);
unsigned	R_LightCode (double light);

void R_SetFullbrightScale (float scale);
void R_SetUnlitColors (bool floors);
void R_BuildMips (texture_t *tx, bool fence);

// r_image.c
void	R_InitImageTables (void);	// R_InitPalette's, before a loader's thread starts
const float	*R_SrgbLightTable (void);	// an sRGB byte's linear light, a channel to the fourth
byte	*R_LoadTGA (const char *path, int *width, int *height);
pixel_t	R_RGBA8Pixel (const byte *rgba);
void	R_ImagePixels (const byte *rgba, int w, int h, pixel_t *out, int outw, int outh, bool cutout);
pixel_t	R_AveragePixels (const pixel_t *p, int n);

// r_partimage.c and r_partdraw.c: scripted particles
#define	PART_LEVELS	12
#define	PART_WHITE	68719476736.0f		// 512^4: SDR white's light, as R_SrgbLightTable has it

typedef struct
{
	char	name[MAX_QPATH];
	int		numlevels;
	byte	*levels[PART_LEVELS];		// sRGB texels and their alpha, top row first
	int		lw[PART_LEVELS], lh[PART_LEVELS];
} partimage_t;

const partimage_t	*R_PartImage (int index);
void	R_DrawPartScene (void);		// r_scene.particles, into the view

// r_textures.c: TGA files in place of a map's textures
extern cvar_t	r_externaltextures;
void R_TexturesInit (void);
void R_LoadTextureOverride (texture_t *tx, const char *modelname, struct arena_s *arena);
void R_BuildTexturePixels (texture_t *tx, struct arena_s *arena);
void R_LightLiquids (void);		// after d_unlitfloors or d_glowscale changed
void R_LightModelLiquids (model_t *mod);	// one model's, loaded as they changed

// the texels to draw tx with in place of its own, or NULL
static inline const pixel_t *R_TextureOverride (const texture_t *tx, int mip)
{
	return r_externaltextures.value ? tx->rgb[mip] : NULL;
}

// r_worldspawn.c: what the map's worldspawn entity says
typedef struct
{
	char	sky[MAX_QPATH];		// the skybox's name
	char	fog[128];			// as r_fog has it
	float	skyfog;
	bool	hasskyfog;
} worldspawn_t;

extern worldspawn_t	r_worldspawn;
void R_ParseWorldspawn (char *entities);

// r_skybox.c: the skybox's faces, rt bk lf ft up dn, each r_skyboxsize square;
// NULL to draw the sky's texture
#define SKYBOX_FACES	6

extern pixel_t	*r_skyfaces;
extern int		r_skyboxsize;
void R_SkyboxInit (void);
void R_CheckSkybox (bool reload);

// r_fog.c: the frame's fog, if r_fogactive (R_SetupFog); R_DrawFog fogs what
// is drawn before translucency by the depth buffer, R_FogPixel and simd_fogspan
// with d_fog what is drawn after it
extern bool			r_fogactive;
extern simd_fog_t	d_fog;
void	R_FogInit (void);
void	R_SetupFog (void);
void	R_DrawFog (void);
pixel_t	R_FogPixel (pixel_t p, float zi);

// r_lightdata.c
typedef struct
{
	const byte	*decoupled;		// DECOUPLED_LM: a dlminfo_t per face
	const byte	*shifts;		// LMSHIFT: a byte per face
	const byte	*offsets;		// LMOFFSET: an int per face
	const byte	*styles;		// LMSTYLE: stylesperface bytes per face
	int			stylesperface;
} facelumps_t;

// r_fence.c
#define PIXEL_TRANSPARENT	0x80000000u		// a cut-out texel in a fence surface's cache block
#define PIXEL_SKY			0x40000000u		// a sky pixel of the view while there is fog, until R_DrawFog

void R_ClearFences (void);
void R_AddFence (msurface_t *surf);
void R_DrawFences (void);

// translucent surfaces and alias models: blended in after the models, back to
// front; alpha is of 256
int R_EntityAlpha (const entity_t *ent);
int R_SurfaceAlpha (const entity_t *ent, const msurface_t *surf);
void R_AddTranslucent (msurface_t *surf, int alpha);
void R_AddTranslucentModel (model_t *model);
void R_AddTranslucentEntity (entity_t *ent);
void R_DrawTranslucent (void);
void R_DrawRings (void);		// r_scene's, r_ring.c
void R_DrawAliasEntity (void);
void R_RotateBmodel (void);
void R_TransformFrustum (void);
void R_EntityViewVectors (const entity_t *ent, vec3_t right, vec3_t up, vec3_t forward);
void R_EntityModelView (const entity_t *ent, vec3_t org, vec3_t right, vec3_t up, vec3_t forward);

void R_LightDataInit (void);
void R_LoadLightData (model_t *mod, bspfile_t *bsp);
void R_FindFaceLumps (bspfile_t *bsp, int numfaces, facelumps_t *lumps);
void R_SetFaceLightmap (model_t *mod, msurface_t *surf, const bspface_t *face, const facelumps_t *lumps,
	int facenum, const double texmins[2], const double texmaxs[2]);

// r_lightmode 1: linear light in RGB, 128 << 8 is 1.0, clamped keeping the hue
// at the brightest a pixel holds (vid.h)
#define LIGHT_ONE		(128 << 8)
#define LIGHT_MAX		(15 * LIGHT_ONE)

extern cvar_t	r_lightmode;
extern cvar_t	r_dlight_scale;		// dynamic lights' light times this
extern cvar_t	r_lerpframes;
extern cvar_t	r_lerpmuzzlehack;
void R_DlightColor (const dlight_t *dl, float color[3]);

// a value as an int, saturated at the int's range (NaN 0), as ARM converts:
// a sliver of a triangle has steps past it, whose plain conversion C leaves
// undefined
static inline int R_SaturateInt (double v)
{
	if (v >= (double)INT_MAX)
		return INT_MAX;
	if (v <= (double)INT_MIN)
		return INT_MIN;
	return v == v ? (int)v : 0;
}

// lit texel color: a color times light, with 15 fraction bits. A fullbright
// color is never darker than its floor, but brighter light still brightens
// it. The same as simd_litrow_rgb.
static inline pixel_t R_LitColor (pixel_t color, pixel_t floor, unsigned r, unsigned g, unsigned b)
{
	r = (RGB30_R (color) * r) >> 15;
	g = (RGB30_G (color) * g) >> 15;
	b = (RGB30_B (color) * b) >> 15;
	if (r < RGB30_R (floor))
		r = RGB30_R (floor);
	if (g < RGB30_G (floor))
		g = RGB30_G (floor);
	if (b < RGB30_B (floor))
		b = RGB30_B (floor);
	return RGB30 (r > 1023 ? 1023 : r, g > 1023 ? 1023 : g, b > 1023 ? 1023 : b);
}

// the palette color's
static inline pixel_t R_LitPixel (int index, unsigned r, unsigned g, unsigned b)
{
	return R_LitColor (d_pal30[index], d_pal30_floor[index], r, g, b);
}

// a pixel's light times r_fullbright_scale (d_glowscale), each channel at most 1023
static inline pixel_t R_GlowPixel (pixel_t p)
{
	unsigned	r = RGB30_R (p) * (unsigned)d_glowscale >> 15;
	unsigned	g = RGB30_G (p) * (unsigned)d_glowscale >> 15;
	unsigned	b = RGB30_B (p) * (unsigned)d_glowscale >> 15;

	return RGB30 (r < 1023 ? r : 1023, g < 1023 ? g : 1023, b < 1023 ? b : 1023);
}
extern cvar_t	r_drawflat;
extern cvar_t	r_ambient;
extern cvar_t	r_numsurfs;
extern cvar_t	r_numedges;

#define XCENTERING	(1.0 / 2.0)
#define YCENTERING	(1.0 / 2.0)

#define CLIP_EPSILON		0.001

#define BACKFACE_EPSILON	0.01

//===========================================================================

#define	DIST_NOT_SET	98765

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct clipplane_s
{
	vec3_t		normal;
	float		dist;
	struct		clipplane_s	*next;
	byte		leftedge;
	byte		rightedge;
	byte		reserved[2];
} clipplane_t;

extern	clipplane_t	view_clipplanes[4];
void R_ViewFrustum (const vec3_t right, const vec3_t up, const vec3_t forward, const vec3_t org, clipplane_t planes[4]);

//=============================================================================
// the bands of the view
//
// The edge pipeline draws the view in horizontal bands, on the worker threads
// at once (r_band.c): each walks the world and the brush entities, clipping to
// the view as a whole and keeping what falls on its scan lines, and scans
// those lines into the spans of its surfaces. A band has everything the
// pipeline changes while it runs; what they share is only read, and the
// spans are drawn after all have run. A band's lines come out as they would
// with one band for the view.

#define MAX_BANDS		64

// r_edgestarts of an edge none of the band's lines has
#define EDGE_OUTSIDE	0xFFFFFFFFu

typedef struct
{
	msurface_t	*surf;
	entity_t	*entity;
	int			alpha;				// of 256: below is translucent, 256 a fence
} rafter_t;

typedef struct rband_s
{
	int			top, bottom;		// the scan lines, bottom not included

// the view, in the space of the model being drawn
	vec3_t		modelorg, vpn, vright, vup;
	clipplane_t	clipplanes[4];		// the view's edges, which faces are clipped to
	mplane_t	cullplanes[2];		// above and below the lines (world space): what
									//  is wholly beyond them isn't the band's
	int			cullindexes[2][6];	// as pfrustum_indexes, for cullplanes
	int			cullflags;			// 16 and 32 for those it has: where it doesn't
									//  reach the view's top and bottom
	entity_t	*entity;			// being drawn
	bool		insubmodel;			// a brush entity
	mvertex_t	*vertbase;			// its model's vertexes
	vec3_t		entorigin;
	float		entity_rotation[3][3];
	int			clipflags;			// the clip planes a brush entity needs

// the world walk (sized for world, R_BandWorld)
	model_t		*world;
	byte		*surfvisible;		// a bit per world surface, set by the leaves walked
	int			*leafkeys;			// each leaf's key this frame, for brush models in it
	unsigned	*edgecache;			// each world edge's offset into edges, or
									//  FULLY_CLIPPED_CACHED and the pass
	unsigned	pass;				// counts the band's runs
	float		*edgenearzi;		// a FULLY_CLIPPED_CACHED edge's 1/z: a face's
									//  nearzi is the same whatever it met first
	int			currentkey, currentbkey;

// a brush entity's polygons clipped through the world (r_bsp.c)
	mvertex_t	*bverts;
	bedge_t		*bedges;
	int			numbverts, numbedges, maxbverts, maxbedges;
	mvertex_t	*frontenter, *frontexit;
	bool		makeclippededge;

// a face's edges being clipped and emitted (r_draw.c)
	medge_t		*pedge;
	medge_t		tedge;				// a dummy, for the edge caching to write to
	unsigned	cacheoffset;
	float		cachenearzi;		// with cacheoffset FULLY_CLIPPED_CACHED
	bool		leftclipped, rightclipped, makeleftedge, makerightedge, nearzionly;
	mvertex_t	leftenter, leftexit, rightenter, rightexit;
	bool		emitted;
	float		nearzi;
	float		u1, v1, lzi1;
	int			ceilv1;
	bool		lastvertvalid;

// the edges and surfaces made (r_edge.c)
	edge_t		*edges, *edge_p, *edge_max;
	uint32_t	*edgestarts;		// by edge: the line it starts on (above the band
									//  for one it starts on its first) times two, plus
									//  one for a trailing edge; or EDGE_OUTSIDE
	int			maxedges;
	surf_t		*surfmem;			// surfaces points one before it
	surf_t		*surfaces, *surface_p, *surf_max;
	int			maxsurfs;
	edge_t		**removeedges;		// by line: the edges that end there
	uint32_t	*sortededges, *sortbuffer;	// indices into edges (R_SortNewEdges)
	int			maxsortededges;
	int			*linestart;			// by line: where its edges start in sortededges
	int			*columnstart;
	int			height, width;		// of the lines and columns these are for

// the scan (r_edge.c)
	espan_t		*spans, *span_p, *max_span_p;
	int			maxspans;
	edge_t		edge_head, edge_tail, edge_aftertail, edge_sentinel;
	int			edge_head_u_shift20, edge_tail_u_shift20;
	int			current_iv;
	float		fv;
	int			bmodelactive;

// the fences and translucent surfaces met, drawn after the world (r_fence.c)
	rafter_t	*afters;
	int			numafters, maxafters;

// what ran out of room: the band is drawn again with more (r_band.c)
	bool		outofedges, outofsurfaces, outofspans, outofbmodel, outofafters;

	int			faceclip, polycount;	// counts, for r_speeds
	double		time;				// the last run took, in seconds
} rband_t;

// TransformVector in the band's view
static inline void R_BandTransform (const rband_t *b, const vec3_t in, vec3_t out)
{
	out[0] = DotProduct (in, b->vright);
	out[1] = DotProduct (in, b->vup);
	out[2] = DotProduct (in, b->vpn);
}

void R_RenderWorld (rband_t *b);
void R_DrawBEntities (rband_t *b);
void R_RotateBandBmodel (rband_t *b);
void R_TransformBandFrustum (rband_t *b);
void R_RenderFace (rband_t *b, msurface_t *fa, int clipflags);
void R_RenderBmodelFace (rband_t *b, bedge_t *pedges, msurface_t *psurf);
void R_DrawSubmodelPolygons (rband_t *b, model_t *pmodel, int clipflags);
void R_DrawSolidClippedSubmodelPolygons (rband_t *b, model_t *pmodel);
void R_BeginEdgeFrame (rband_t *b);
void R_ScanEdges (rband_t *b);
void D_DrawSurfaces (rband_t *bands, int numbands);
void D_FinishSurfaces (void);
int R_BmodelCheckBBox (const entity_t *ent, const float *minmaxs, const rband_t *b);
void R_BandWorldView (rband_t *b);
bool R_GrowBandBModelClip (rband_t *b);
void R_AddAfter (rband_t *b, msurface_t *surf, int alpha);
void R_MergeAfters (void);

extern rband_t	r_bands[MAX_BANDS];
extern int		r_numbands;

void R_SetEdgeSize (int width, int height);
void R_SetBandRoom (int numedges, int numsurfs);
void R_RunBands (int numbands);

void R_StoreStaticEntities (void);
void R_MarkEfragNodes (mleaf_t *leaf);

//=============================================================================

extern	mplane_t	screenedge[4];

extern	vec3_t	r_origin;

extern	vec3_t	r_entorigin;


extern	int		r_visframecount;

//=============================================================================


void R_DrawSprite (void);
void R_SetSkyFrame (void);
texture_t *R_TextureAnimation (texture_t *base);

void R_AliasDrawModel (alight_t *plighting);

extern int	c_faceclip;
extern int	r_polycount;
extern int	r_wholepolycount;


extern int		*pfrustum_indexes[4];

// !!! if this is changed, it must be changed in asm_draw.h too !!!
#define	NEAR_CLIP	0.01

extern int			ubasestep, errorterm, erroradjustup, erroradjustdown;

extern fixed16_t	sadjust, tadjust;
extern fixed16_t	bbextents, bbextentt;

extern vec3_t			sbaseaxis[3], tbaseaxis[3];

//=========================================================
// Alias models
//=========================================================

#define MAXALIASVERTS		2000	// TODO: tune this
#define ALIAS_Z_CLIP_PLANE	5

extern int				numverts;
extern int				numtriangles;
extern float			leftclip, topclip, rightclip, bottomclip;
extern int				r_acliptype;
extern finalvert_t		*pfinalverts;
extern auxvert_t		*pauxverts;

bool R_AliasCheckBBox (void);

//=========================================================
// turbulence stuff

#define	AMP		8*0x10000
#define	AMP2	3
#define	SPEED	20

//=========================================================
// particle stuff

void R_DrawParticles (void);
void R_InitParticles (void);
void R_ClearParticles (void);
void R_ReadPointFile_f (void);

extern int		r_amodels_drawn;

void R_SetWarpTable (int size);

extern	int	screenwidth;

extern float		aliasxscale, aliasyscale, aliasxcenter, aliasycenter;
extern float		r_aliastransition, r_resfudge;


void R_AliasClipTriangle (mtriangle_t *ptri);

extern float	r_time1;
extern float	dp_time1, dp_time2, db_time1, db_time2, rw_time1, rw_time2;
extern float	se_time1, se_time2, de_time1, de_time2, dv_time1, dv_time2;
extern int		r_frustum_indexes[4*6];
extern int r_maxsurfsseen, r_maxedgesseen;
extern bool	r_dowarpold, r_viewchanged;
extern vrect_t	r_viewrect;
extern float	r_viewaspect;
void R_SetViewRect (const vrect_t *vrect, float aspect);

extern mleaf_t	*r_viewleaf, *r_oldviewleaf;

extern vec3_t	r_emins, r_emaxs;
extern mnode_t	*r_pefragtopnode;
extern int		r_dlightframecount;

void R_StoreEfrags (efrag_t **ppefrag);
void R_TimeRefresh_f (void);
void R_TimeGraph (void);
void R_PrintAliasStats (void);
void R_PrintTimes (void);
void R_PrintDSpeeds (void);
void R_AnimateLight (void);
int R_LightPoint (vec3_t p, vec3_t color);
void R_SetupFrame (void);
void R_SplitEntityOnNode2 (mnode_t *node);
void R_MarkLights (dlight_t *light, unsigned bit, mnode_t *node);
