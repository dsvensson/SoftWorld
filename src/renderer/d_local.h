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


void D_DrawSpans (espan_t *pspans);
void D_DrawZSpans (espan_t *pspans);
void Turbulent8 (espan_t *pspan);
int D_SurfaceMipLevel (msurface_t *surface, int miplevel);

void D_DrawSkyScans (espan_t *pspan);

extern byte		*d_turbsource;	// the 64x64 texture of a turbulent surface

extern void (*prealspandrawer)(void);
surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel);
// a fence surface's clipped, projected polygon, with 1/z gradients set;
// transformed_org is the view origin in the model's space
void D_DrawFence (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump, float nearzi);
void D_DrawTranslucentFace (msurface_t *surf, const vec3_t transformed_org, emitpoint_t *pverts, int nump,
	float nearzi, int alpha);
// the spans of a convex polygon on the screen, clockwise; false if it covers no scan line
void D_DrawFencePolygon (emitpoint_t *pverts, int nump);
void D_DrawBlendedPolygon (emitpoint_t *pverts, int nump, int alpha, bool turb);
// one color over what is there, depth tested and not written
void D_DrawFlatPolygon (emitpoint_t *pverts, int nump, pixel_t color, int alpha);
void D_DrawBlendedSpans (sspan_t *pspan, int alpha, bool turb);

// translucency: how opaque the alias model being drawn is, of 256; a scratch
// row for a span's texels before they are blended in, at least count long
extern int	d_alpha;
pixel_t	*D_BlendRow (int count);

// src over dst, src weighted a of 256
static inline pixel_t D_BlendPixel (pixel_t src, pixel_t dst, int a)
{
	unsigned	ia = 256 - (unsigned)a;

	return ((((src & 1023) * (unsigned)a + (dst & 1023) * ia) >> 8)) |
		(((((src >> 10) & 1023) * (unsigned)a + ((dst >> 10) & 1023) * ia) >> 8) << 10) |
		(((((src >> 20) & 1023) * (unsigned)a + ((dst >> 20) & 1023) * ia) >> 8) << 20);
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

extern int		d_minmip;
extern float	d_scalemip[3];

extern void (*d_drawspans) (espan_t *pspan);
