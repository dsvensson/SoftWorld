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
// simd_bind_scalar.c -- simd.h on the plain C kernels; link-time code generation
// inlines these

#include "simd_backends.h"

void simd_zspan (float *dest, int count, float zi, float step)
{
	Simd_Scalar_ZSpan (dest, count, zi, step);
}

void simd_texspan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	Simd_Scalar_TexSpan (dest, map, src, srcwidth, u, v, count);
}

void simd_turbspan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	Simd_Scalar_TurbSpan (dest, map, src, palette, turb, u, v, count);
}

void simd_litrow_colormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	Simd_Scalar_LitRowColormap (dest, src, colormap, light, step, count);
}

void simd_litrow_rgb (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	Simd_Scalar_LitRowRGB (dest, src, palette, floor, light, step, count);
}

void simd_aliasspan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	Simd_Scalar_AliasSpan (dest, zbuf, tex, sfrac, tfrac, light, zi, count, map);
}

void simd_expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
}

void simd_copy_stream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	Simd_Scalar_CopyStream (dest, destpitch, src, srcpitch, rowbytes, rows);
}
