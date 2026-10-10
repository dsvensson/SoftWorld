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
// simd_backends.h -- the backends behind simd.h, by name, for the bind files
// and test_simd. See simd.h for what each kernel does.

#include "simd.h"

// a helper inlined into the kernels that call it: the span steppers were
// left calls of their own, and the textured spans took a fifth longer for it
#if defined(_MSC_VER) && !defined(__clang__)
#define SIMD_INLINE	static __forceinline
#else
#define SIMD_INLINE	static inline __attribute__((always_inline))
#endif

void	Simd_Scalar_ZSpan (float *dest, int count, float zi, float step);
void	Simd_Scalar_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
			int u, int v, int count);
void	Simd_Scalar_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
			const int *turb, int u, int v, int count);
void	Simd_Scalar_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
			const int *turb, int u, int v, int count);
void	Simd_Scalar_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
			int light, int step, int count);
void	Simd_Scalar_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
			const uint32_t *floor, const int light[3], const int step[3], int count);
void	Simd_Scalar_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
			const int light[3], const int step[3], int count);
void	Simd_Scalar_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
			int light, int zi, int count, const simd_aliasmap_t *map);
void	Simd_Scalar_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
			int alpha, int count);
void	Simd_Scalar_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
			const float *const src[3], const float *const mul[3], int count);
void	Simd_Scalar_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
			const simd_fog_t *fog);
void	Simd_Scalar_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);
void	Simd_Scalar_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);

void	Simd_V4_ZSpan (float *dest, int count, float zi, float step);
void	Simd_V4_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
			int u, int v, int count);
void	Simd_V4_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
			const int *turb, int u, int v, int count);
void	Simd_V4_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
			const int *turb, int u, int v, int count);
void	Simd_V4_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
			int light, int step, int count);
void	Simd_V4_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
			const uint32_t *floor, const int light[3], const int step[3], int count);
void	Simd_V4_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
			const int light[3], const int step[3], int count);
void	Simd_V4_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
			int light, int zi, int count, const simd_aliasmap_t *map);
void	Simd_V4_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
			int alpha, int count);
void	Simd_V4_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
			const float *const src[3], const float *const mul[3], int count);
void	Simd_V4_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
			const simd_fog_t *fog);
void	Simd_V4_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);
void	Simd_V4_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);

void	Simd_V3_ZSpan (float *dest, int count, float zi, float step);
void	Simd_V3_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
			int u, int v, int count);
void	Simd_V3_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
			const int *turb, int u, int v, int count);
void	Simd_V3_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
			const int *turb, int u, int v, int count);
void	Simd_V3_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
			int light, int step, int count);
void	Simd_V3_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
			const uint32_t *floor, const int light[3], const int step[3], int count);
void	Simd_V3_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
			const int light[3], const int step[3], int count);
void	Simd_V3_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
			int light, int zi, int count, const simd_aliasmap_t *map);
void	Simd_V3_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
			int alpha, int count);
void	Simd_V3_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
			const float *const src[3], const float *const mul[3], int count);
void	Simd_V3_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
			const simd_fog_t *fog);
void	Simd_V3_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);
void	Simd_V3_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);

void	Simd_M3_ZSpan (float *dest, int count, float zi, float step);
void	Simd_M3_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
			int u, int v, int count);
void	Simd_M3_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
			const int *turb, int u, int v, int count);
void	Simd_M3_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
			const int *turb, int u, int v, int count);
void	Simd_M3_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
			int light, int step, int count);
void	Simd_M3_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
			const uint32_t *floor, const int light[3], const int step[3], int count);
void	Simd_M3_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
			const int light[3], const int step[3], int count);
void	Simd_M3_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
			int light, int zi, int count, const simd_aliasmap_t *map);
void	Simd_M3_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
			int alpha, int count);
void	Simd_M3_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
			const float *const src[3], const float *const mul[3], int count);
void	Simd_M3_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
			const simd_fog_t *fog);
void	Simd_M3_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
			int scale, int transparent);
void	Simd_M3_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
			size_t rowbytes, int rows);
