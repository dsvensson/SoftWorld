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
// d_local.h:  private rasterization driver defs

#include "r_local.h"

//
// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
//
#define SCANBUFFERPAD		0x1000

#define R_SKY_SMASK	0x007F0000
#define R_SKY_TMASK	0x007F0000

#define DS_SPAN_LIST_END	-128

#define SURFCACHE_SIZE_AT_320X200	600*1024

typedef struct surfcache_s
{
	struct surfcache_s	*next;
	struct surfcache_s 	**owner;		// NULL is an empty chunk of memory
	int					lightadj[MAXLIGHTMAPS]; // checked for strobe flush
	int					dlight;
	int					lightcount;	// light values kept after the texels, 0 if none are yet
	unsigned			batch;		// the batch of surfaces that last used it (D_BeginSurfaceBatch)
	int					framedrawn;	// the frame its texels were last drawn for
	int					size;		// including header
	unsigned			width;
	unsigned			height;		// DEBUG only needed for debug
	float				mipscale;
	struct texture_s	*texture;	// checked for animating textures
	pixel_t				data[1];	// width*height elements
} surfcache_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct sspan_s
{
	int				u, v, count;
} sspan_t;


extern float	scale_for_mip;

extern bool		d_roverwrapped;
extern surfcache_t	*sc_rover;
extern surfcache_t	*d_initial_rover;

extern float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

extern fixed16_t	sadjust, tadjust;
extern fixed16_t	bbextents, bbextentt;


simd_texmap_t D_SpanTexmap (void);		// of the current surface, from the d_ gradients
void D_DrawSpans (espan_t *pspan, const simd_texmap_t *map, const pixel_t *block, int blockwidth);
void D_DrawZSpans (espan_t *pspan, const simd_texmap_t *map);
// texels, if not NULL, are a TGA file's 64x64 in place of texture's
void Turbulent8 (espan_t *pspan, const simd_texmap_t *map, const byte *texture, const pixel_t *texels);
int D_SurfaceMipLevel (msurface_t *surface, int miplevel);

void D_DrawSkyScans (espan_t *pspan);
void D_DrawSkyboxScans (espan_t *pspan);

extern byte		*d_turbsource;	// the 64x64 texture of a turbulent surface
extern const pixel_t	*d_turbsource30;	// a TGA file's texels in its place, or NULL

extern void (*prealspandrawer)(void);
surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel);

// surfaces cached in batches: prepared one by one, then their blocks drawn
// together, on any threads (D_DrawSurfaces)
typedef enum
{
	CACHE_READY,		// the block's texels are right
	CACHE_DRAW,			// they are to be drawn first
	CACHE_TAKEN			// an earlier surface of the batch needs the block, or its room
} cacheprep_t;

void		D_BeginSurfaceBatch (void);
cacheprep_t	D_PrepareCacheSurface (msurface_t *surface, int miplevel, surfcache_t **pcache, drawsurf_t *draw);
int			D_DrawCacheSurface (const drawsurf_t *draw);	// returns the texels drawn
// a fence surface's clipped, projected polygon, with 1/z gradients set;
// transformed_org is the view origin in the model's space
void D_DrawFence (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump, float nearzi);
void D_DrawTranslucentFace (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump,
	float nearzi, int alpha);
// the spans of a convex polygon on the screen, clockwise, held to rect: a line
// each, in order, then DS_SPAN_LIST_END; spans has room for rect's height and
// the end, pverts for one more vertex; false if it covers no scan line
bool D_PolygonSpans (emitpoint_t *pverts, int nump, sspan_t *spans, const vrect_t *rect);
// the same, drawn as a fence, blended
void D_DrawFencePolygon (emitpoint_t *pverts, int nump);
void D_DrawBlendedPolygon (emitpoint_t *pverts, int nump, int alpha, bool turb);
// one color over what is there, depth tested and not written
void D_DrawFlatPolygon (emitpoint_t *pverts, int nump, pixel_t color, int alpha);
void D_DrawBlendedSpans (sspan_t *pspan, int alpha, bool turb);

// translucency: how opaque the alias model being drawn is, of 256; a scratch
// row for a span's texels before they are blended in, at least count long
extern int	d_alpha;
pixel_t	*D_BlendRow (int count);

// a channel of src over dst, as simd_blendspan blends it: in linear light, a
// channel being the fourth root of its light
static inline unsigned D_BlendChannel (unsigned s, unsigned d, float a, float ia)
{
	float		fs = (float)s, fd = (float)d;
	unsigned	c;

	fs *= fs;
	fs *= fs;
	fd *= fd;
	fd *= fd;
	c = (unsigned)(sqrtf (sqrtf ((fs * a + fd * ia) * (1.0f / 256.0f))) + 0.5f);
	return c < 1023 ? c : 1023;
}

// src over dst, src weighted a of 256
static inline pixel_t D_BlendPixel (pixel_t src, pixel_t dst, int a)
{
	float	fa = (float)a, fia = (float)(256 - a);

	return D_BlendChannel (src & 1023, dst & 1023, fa, fia)
		| (D_BlendChannel ((src >> 10) & 1023, (dst >> 10) & 1023, fa, fia) << 10)
		| (D_BlendChannel ((src >> 20) & 1023, (dst >> 20) & 1023, fa, fia) << 20);
}


// 1/z of the nearest thing drawn at each pixel; 0 is infinitely far
extern float *d_pzbuffer;
extern unsigned int d_zwidth;

// alias models step 1/z in 31 bit fixed point
#define ALIAS_ZI_TO_FLOAT	(1.0f / 2147483648.0f)

extern int	*d_pscantable;
extern int	*d_scantable;

extern int	d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

extern int	d_y_aspect_shift, d_pix_min, d_pix_max;

extern pixel_t	*d_viewbuffer;

extern float	**zspantable;

void D_SetWarpSize (int width, int height, int scale);
void D_SetPolysetSize (int height);
void D_SetSpriteSize (int height);
void D_SetBatchSize (int height);

// a sprite's mapping, as D_SpriteSpan draws a span of it
typedef struct
{
	const byte	*pixels;
	int			width;
	float		sdivzorigin, sdivzstepu, sdivzstepv;
	float		tdivzorigin, tdivzstepu, tdivzstepv;
	float		ziorigin, zistepu, zistepv;
	fixed16_t	sadjust, tadjust, bbextents, bbextentt;
} d_spritemap_t;

// a span of a sprite on line v, depth tested and written (d_sprite.c)
void D_SpriteSpan (const d_spritemap_t *map, int u, int v, int count);

// what's kept in the batch D_BeginBatch began, if one is (d_batch.c), in
// strips of lines: thin, as a view model is in few of the view's lines and
// Sys_Parallel hands the strips out as threads come free
#define D_STRIP_LINES	8
bool D_Keeping (void);
int D_KeepAliasMap (const simd_aliasmap_t *map);
int D_KeepSpriteMap (const d_spritemap_t *map);
void D_KeepAliasSpan (int v, pixel_t *pdest, float *pz, const byte *ptex, int sfrac, int tfrac, int light, int zi,
	int count, int map);
void D_KeepAliasPixel (int v, pixel_t *pdest, float *pz, pixel_t color, float z);
void D_KeepSpriteSpan (int u, int v, int count, int map);
// lines lines of a particle's square, width wide, from line v (d_part.c)
void D_KeepParticle (int v, pixel_t *pdest, float *pz, pixel_t color, float zi, int width, int lines);
void D_ParticleLines (pixel_t *pdest, float *pz, pixel_t color, float zi, int width, int lines);

extern int		d_minmip;
extern float	d_scalemip[3];

