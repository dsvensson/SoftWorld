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
// simd.h -- the inner loops of the renderer, each a whole span or row. The
// build picks one backend (simd_bind_<arch>.c); every backend gives the same
// results bit for bit as the scalar one, which test_simd checks.

#include "q_types.h"

#include <stddef.h>

// 1/z along a span: dest[i] = zi + i * step, in float
void	simd_zspan (float *dest, int count, float zi, float step);

// how a surface's texture lies on the screen: s/z, t/z and 1/z at pixel
// (u, v) are origin + u*stepu + v*stepv; s and t (16.16) are those over 1/z
// plus the adjust, clamped to 0 .. extent
typedef struct
{
	float	sdivzorigin, sdivzstepu, sdivzstepv;
	float	tdivzorigin, tdivzstepu, tdivzstepv;
	float	ziorigin, zistepu, zistepv;
	int		sadjust, tadjust;
	int		sextent, textent;
} simd_texmap_t;

// a span of count pixels from (u, v) of a surface cache block, src with
// srcwidth pixels per row. s and t are exact every 8 pixels and stepped in
// between, as the original span drawer does.
void	simd_texspan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
			int u, int v, int count);

// a span of a turbulent (water) surface: src is its 64x64 8 bit texture, with
// 3 bytes readable after it, drawn through palette; turb is the wave, read at
// 0 .. 254. s and t are exact every 16 pixels.
void	simd_turbspan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
			const int *turb, int u, int v, int count);

// a row of a surface lit through a colormap: texel j (0 .. count-1) gets
// light + (count - 1 - j) * step, and becomes colormap[(light & 0xFF00) + texel]
void	simd_litrow_colormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
			int light, int step, int count);

// a row of a surface lit in RGB: texel j gets light[c] + (count - 1 - j) *
// step[c] per channel (negative is 0), 32768 being 1.0. Each channel of the
// palette color (RGB30) times its light >> 15, at least the same channel of
// floor (RGB30, 0 for colors light may darken), at most 1023.
void	simd_litrow_rgb (uint32_t *dest, const byte *src, const uint32_t *palette,
			const uint32_t *floor, const int light[3], const int step[3], int count);

// how the spans of an alias model triangle step: per pixel, 1/z and light by
// their steps, and the skin by stepwhole texels plus the carries of the 16 bit
// s and t fractions (a t carry is one skin row)
typedef struct
{
	int				zistep, lightstep;
	int				stepwhole, sfracstep, tfracstep;	// fractions 0 .. 0xFFFF
	int				skinwidth;
	const byte		*remap;			// skin colors to palette indices (256)
	const uint32_t	*colormap;		// classic lighting: colormap[index + (light & 0xFF00)]
	const uint32_t	*palette;		// else RGB: the palette and the floor as in
	const uint32_t	*floor;			// simd_litrow_rgb, the light
	unsigned		tint[3];		// ((255 << 6) - light) * tint >> 6
} simd_aliasmap_t;

// count pixels of an alias model span from tex (its s and t fractions sfrac
// and tfrac, 0 .. 0xFFFF): a pixel whose 1/z, zi * 2^-31, is at least the z
// buffer's is written, with its 1/z
void	simd_aliasspan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
			int light, int zi, int count, const simd_aliasmap_t *map);

// 8 bit texels through a palette, each written scale times: dest[i*scale + k]
// = palette[src[i]]; texels equal to transparent (if not -1) are skipped
void	simd_expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);

// rows of an image copied to memory the CPU only writes, such as a mapped GPU
// texture: rowbytes of each of rows rows, the rows destpitch and srcpitch
// bytes apart
void	simd_copy_stream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);
