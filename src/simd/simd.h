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

// a span of a surface cache block: dest[i] = src[((s + i*sstep) >> 16) +
// ((t + i*tstep) >> 16) * srcwidth], count at most 16
void	simd_texspan (uint32_t *dest, const uint32_t *src, int srcwidth,
			int s, int t, int sstep, int tstep, int count);

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

// 8 bit texels through a palette, each written scale times: dest[i*scale + k]
// = palette[src[i]]; texels equal to transparent (if not -1) are skipped
void	simd_expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);

// rows of an image copied to memory the CPU only writes, such as a mapped GPU
// texture: rowbytes of each of rows rows, the rows destpitch and srcpitch
// bytes apart
void	simd_copy_stream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);
