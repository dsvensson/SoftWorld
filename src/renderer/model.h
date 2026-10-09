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

#include "modelgen.h"
#include "spritegn.h"
#include "bspfile.h"
#include "mathlib.h"
#include "q_types.h"
#include "vid.h"

/*

d*_t structures are on-disk representations
m*_t structures are in-memory

*/

// entity effects

#define	EF_BRIGHTFIELD			1
#define	EF_MUZZLEFLASH 			2
#define	EF_BRIGHTLIGHT 			4
#define	EF_DIMLIGHT 			8
#define	EF_FLAG1	 			16
#define	EF_FLAG2	 			32
#define EF_BLUE					64
#define EF_RED					128

/*
==============================================================================

BRUSH MODELS

==============================================================================
*/


//
// in memory representation
//
// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	vec3_t		position;
} mvertex_t;

#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2


typedef struct texture_s
{
	char		name[16];
	unsigned	width, height;
	int			anim_total;				// total tenths in sequence ( 0 = no)
	int			anim_min, anim_max;		// time for this frame min <=time< max
	struct texture_s *anim_next;		// in the animation sequence
	struct texture_s *alternate_anims;	// bmodels in frmae 1 use these
	unsigned	offsets[MIPLEVELS];		// four mip maps stored
	// a TGA file's texels in place of the map's (r_textures.c), as large, cut
	// out by PIXEL_TRANSPARENT. A liquid's are 64x64 with no mip levels: rgb[0]
	// as drawn, its fullbright light (glow[0]) raised to its floor in
	// r_lightmode 1 (R_LightLiquids), from the file's at rgb[1]. NULL if there
	// is none.
	pixel_t		*rgb[MIPLEVELS];
	pixel_t		*glow[MIPLEVELS];		// their fullbright light, NULL for none
	// the map's own texels as pixels, for walls in r_lightmode 1: mip levels
	// averaged in linear light, the fullbright light apart from the colors, so
	// a small light fades with its share of a texel rather than going out.
	// NULL for skies and liquids; pixelglow NULL without fullbright colors.
	pixel_t		*pixels[MIPLEVELS];
	pixel_t		*pixelglow[MIPLEVELS];
} texture_t;


#define	SURF_PLANEBACK		2
#define	SURF_DRAWSKY		4
#define SURF_DRAWSPRITE		8
#define SURF_DRAWTURB		0x10
#define SURF_DRAWTILED		0x20
#define SURF_DRAWBACKGROUND	0x40
#define SURF_DRAWFENCE		0x80		// a fence texture: index 255 is cut out (r_fence.c)
#define SURF_LAVA			0x100		// turbulent liquids other than water, for their
#define SURF_SLIME			0x200		// r_*alpha
#define SURF_TELE			0x400
#define	SURF_NOTEXELS		0x800		// its texture spans nothing one way: no cache block, not drawn

typedef struct
{
	unsigned	v[2];
	// two of the world's faces have it, one each way: the renderer emits it
	// once for both (R_RenderFace's edge cache), as id's qbsp made every edge;
	// other compilers give an edge to three faces or more
	bool		shared;
} medge_t;

typedef struct
{
	float		vecs[2][4];
	float		mipadjust;
	texture_t	*texture;
	int			flags;
} mtexinfo_t;

typedef struct msurface_s
{
	int			visframe;		// should be drawn when node is crossed
	int			fencepass;		// r_fence.c: the edge pass it was added in
	const void	*fenceentity;	// and for which entity

	int			dlightframe;
	unsigned	dlightbits;			// a bit a dynamic light (MAX_DLIGHTS of them)

	mplane_t	*plane;
	int			flags;

	int			firstedge;	// look up in model->surfedges[], negative numbers
	int			numedges;	// are backwards edges
	float		minmaxs[6];	// its vertexes' bounds, as a node's
	
// surface generation data
	struct surfcache_s	*cachespots[MIPLEVELS];

	int			texturemins[2];
	int			extents[2];

	mtexinfo_t	*texinfo;
	
// lighting info: lmwidth x lmheight luxels a style; texture coordinate (s, t)
// falls on luxel lmvecs . (s, t, 1). A vanilla lightmap has a luxel every 16
// texels from texturemins and is used as it is; others (DECOUPLED_LM, LMSHIFT)
// are sampled onto a grid of 1 << lmgridshift texels (r_lightdata.c).
	byte		styles[MAXLIGHTMAPS];
	bool		lmvanilla;
	int			lmgridshift;
	int			lmwidth, lmheight;
	float		lmvecs[2][3];
	byte		*samples;		// [numstyles*lmwidth*lmheight], 128 is 1.0; NULL if unlit
	unsigned short	*samples_rgb;	// the same *3, 2048 is 1.0; NULL without RGB light
} msurface_t;

typedef struct mnode_s
{
// common with leaf
	int			contents;		// 0, to differentiate from leafs
	int			visframe;		// node needs to be traversed if current
	
	float		minmaxs[6];		// for bounding box culling

	struct mnode_s	*parent;
	bool		efragged;		// a static entity is in it or a leaf under it

// node specific
	mplane_t	*plane;
	struct mnode_s	*children[2];	

	unsigned	firstsurface;
	unsigned	numsurfaces;
} mnode_t;



typedef struct mleaf_s
{
// common with node
	int			contents;		// wil be a negative contents number
	int			visframe;		// node needs to be traversed if current

	float		minmaxs[6];		// for bounding box culling

	struct mnode_s	*parent;
	bool		efragged;		// a static entity is in it or a leaf under it

// leaf specific
	byte		*compressed_vis;
	struct efrag_s	*efrags;

	msurface_t	**firstmarksurface;
	int			nummarksurfaces;
	int			key;			// BSP sequence number for leaf's contents
	byte		ambient_sound_level[NUM_AMBIENTS];
} mleaf_t;


/*
==============================================================================

SPRITE MODELS

==============================================================================
*/


// FIXME: shorten these?
typedef struct mspriteframe_s
{
	int		width;
	int		height;
	void	*pcachespot;			// remove?
	float	up, down, left, right;
	byte	pixels[4];
} mspriteframe_t;

typedef struct
{
	int				numframes;
	float			*intervals;
	mspriteframe_t	*frames[1];
} mspritegroup_t;

typedef struct
{
	spriteframetype_t	type;
	mspriteframe_t		*frameptr;
} mspriteframedesc_t;

typedef struct
{
	int					type;
	int					maxwidth;
	int					maxheight;
	int					numframes;
	float				beamlength;		// remove?
	void				*cachespot;		// remove?
	mspriteframedesc_t	frames[1];
} msprite_t;


/*
==============================================================================

ALIAS MODELS

Alias models are position independent, so the cache manager can move them.
==============================================================================
*/

typedef struct
{
	aliasframetype_t	type;
	trivertx_t			bboxmin;
	trivertx_t			bboxmax;
	int					frame;
	char				name[16];
} maliasframedesc_t;

typedef struct
{
	aliasskintype_t		type;
	void				*pcachespot;
	int					skin;
} maliasskindesc_t;

typedef struct
{
	trivertx_t			bboxmin;
	trivertx_t			bboxmax;
	int					frame;
} maliasgroupframedesc_t;

typedef struct
{
	int						numframes;
	int						intervals;
	maliasgroupframedesc_t	frames[1];
} maliasgroup_t;

typedef struct
{
	int					numskins;
	int					intervals;
	maliasskindesc_t	skindescs[1];
} maliasskingroup_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct mtriangle_s {
	int					facesfront;
	int					vertindex[3];
} mtriangle_t;

typedef struct {
	int					model;
	int					stverts;
	int					skindesc;
	int					triangles;
	maliasframedesc_t	frames[1];
} aliashdr_t;

//===================================================================

//
// Whole model
//

typedef enum {mod_brush, mod_sprite, mod_alias} modtype_t;

#define	EF_ROCKET	1			// leave a trail
#define	EF_GRENADE	2			// leave a trail
#define	EF_GIB		4			// leave a trail
#define	EF_ROTATE	8			// rotate (bonus items)
#define	EF_TRACER	16			// green split trail
#define	EF_ZOMGIB	32			// small blood trail
#define	EF_TRACER2	64			// orange split trail + rotate
#define	EF_TRACER3	128			// purple trail
#define	MF_HOLEY	(1 << 14)	// index 255 of its skins is a hole (QuakeSpasm's)

typedef struct model_s
{
	char		name[MAX_QPATH];
	bool	needload;		// bmodels and sprites don't cache normally

	modtype_t	type;
	int			numframes;
	synctype_t	synctype;
	
	int			flags;

//
// volume occupied by the model graphics
//		
	vec3_t		mins, maxs;
	float		radius;

//
// solid volume for clipping (sent from server)
//

//
// brush model
//
	int			firstmodelsurface, nummodelsurfaces;
	int			firstnode;		// the model's head node

	int			numsubmodels;
	dmodel_t	*submodels;

	int			numplanes;
	mplane_t	*planes;

	int			numleafs;		// number of visible leafs, not counting 0
	mleaf_t		*leafs;
	int			numloadedleafs;	// all of leafs, the submodels' too

	int			numvertexes;
	mvertex_t	*vertexes;

	int			numedges;
	medge_t		*edges;

	int			numnodes;
	mnode_t		*nodes;

	int			numtexinfo;
	mtexinfo_t	*texinfo;

	int			numsurfaces;
	msurface_t	*surfaces;

	int			numsurfedges;
	int			*surfedges;

	int			nummarksurfaces;
	msurface_t	**marksurfaces;

	int			numtextures;
	texture_t	**textures;
	texture_t	*skytexture;	// the last named sky*, made the sky at R_NewMap

	byte		*visdata;
	int			vissize;
	int			visbytes;		// a row of visibility bits, for every leaf
	byte		*novis;			// everything visible
	byte		*pvs;			// the last decompressed row
	struct vpsource_s	*vissource;	// to widen it across liquids with, NULL: nothing to widen
	bool		viswidened;		// asked to be (Mod_WidenVis)
	byte		*visrows;		// the rows widened across liquids (BSP_PatchVis), NULL: not yet
	int			*leafrow;		// each leaf's offset in them, -1 none
	byte		*lightdata;		// mono, 128 is 1.0
	int			lightsamples;
	unsigned short	*lightrgb;	// 3 per sample, 2048 is 1.0; NULL without colored light
	char		*entities;

//
// data ownership
//
	struct arena_s	*arena;			// owns everything below; NULL for inline brush submodels
	void		*extradata;		// aliashdr_t or msprite_t

} model_t;

//============================================================================

void	Mod_Init (void);
void	Mod_ClearAll (void);
void	Mod_ForEachTexture (void (*fn) (texture_t *tx));	// of the brush models loaded
model_t *Mod_ForName (char *name, bool crash);
model_t	*Mod_FindLoaded (const char *name);	// NULL if it isn't loaded
// a model from its file's contents, e.g. for tests; false (the reason is
// printed) if it can't be used. Mod_Unload frees what it loaded.
bool	Mod_LoadFromBuffer (model_t *mod, byte *buffer, int size);
void	Mod_Unload (model_t *mod);
// the same, of no name the renderer knows, on any thread (a loader's); NULL if
// it can't be used. Mod_Install makes it the model of its name on the main
// thread, freeing it; Mod_FreeDetached frees it instead.
model_t	*Mod_LoadDetached (const char *name, byte *buffer, int size);
model_t	*Mod_Install (model_t *detached);
void	Mod_FreeDetached (model_t *detached);
// the world's inline models ("*1" ...), from the brush model that is the world
void	Mod_SetWorld (model_t *world);
void	*Mod_Extradata (model_t *mod);	// handles caching

mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model);
byte	*Mod_LeafPVS (mleaf_t *leaf, model_t *model);
// its visibility widened across liquids its vis treated as opaque, or as built
void	Mod_WidenVis (model_t *mod, bool widen);
