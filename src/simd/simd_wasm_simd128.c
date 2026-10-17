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
// simd_wasm_simd128.c -- the kernels for WebAssembly's 128 bit SIMD
// (-msimd128): four 32 bit lanes, as simd_apple_m3.c's NEON, which these
// follow. Same results as simd_scalar.c, bit for bit: multiplies and adds
// apart (WebAssembly has no fused ones), square roots and divisions correctly
// rounded, and float to int saturating, as the scalar casts are compiled.
//
// WebAssembly has no gather: what is looked up in a table is loaded a lane at a
// time, and the arithmetic around it is in lanes. Where that doesn't beat the
// scalar kernel as the compiler builds it, the scalar kernel is used.

#include "simd_backends.h"

#include <string.h>
#include <wasm_simd128.h>

static inline v128_t Simd_W_Iota (void)
{
	return wasm_i32x4_make (0, 1, 2, 3);
}

// four table entries, by four bytes
SIMD_INLINE v128_t Simd_W_LookupBytes (const uint32_t *table, const byte *idx)
{
	return wasm_u32x4_make (table[idx[0]], table[idx[1]], table[idx[2]], table[idx[3]]);
}

// four bytes, zero extended to 32 bit lanes
SIMD_INLINE v128_t Simd_W_Widen (const byte *src)
{
	return wasm_u32x4_extend_low_u16x8 (wasm_u16x8_extend_low_u8x16 (wasm_v128_load32_zero (src)));
}

// sixteen at a time from four running indices, then four, then the last four
// again ending on the last pixel (the same values where they overlap)
void Simd_W_ZSpan (float *dest, int count, float zi, float step)
{
	const v128_t	vzi = wasm_f32x4_splat (zi), vstep = wasm_f32x4_splat (step);
	const v128_t	four = wasm_f32x4_splat (4), sixteen = wasm_f32x4_splat (16);
	v128_t			i0, i1, i2, i3;		// whole numbers, exact
	int				i;

	if (count < 4)
	{
		for (i = 0 ; i < count ; i++)
			dest[i] = zi + (float)i * step;
		return;
	}
	i0 = wasm_f32x4_convert_i32x4 (Simd_W_Iota ());
	i1 = wasm_f32x4_add (i0, four);
	i2 = wasm_f32x4_add (i1, four);
	i3 = wasm_f32x4_add (i2, four);
	for (i = 0 ; i + 16 <= count ; i += 16)
	{
		wasm_v128_store (dest + i, wasm_f32x4_add (vzi, wasm_f32x4_mul (i0, vstep)));
		wasm_v128_store (dest + i + 4, wasm_f32x4_add (vzi, wasm_f32x4_mul (i1, vstep)));
		wasm_v128_store (dest + i + 8, wasm_f32x4_add (vzi, wasm_f32x4_mul (i2, vstep)));
		wasm_v128_store (dest + i + 12, wasm_f32x4_add (vzi, wasm_f32x4_mul (i3, vstep)));
		i0 = wasm_f32x4_add (i0, sixteen);
		i1 = wasm_f32x4_add (i1, sixteen);
		i2 = wasm_f32x4_add (i2, sixteen);
		i3 = wasm_f32x4_add (i3, sixteen);
	}
	for ( ; i + 4 <= count ; i += 4)
	{
		wasm_v128_store (dest + i, wasm_f32x4_add (vzi, wasm_f32x4_mul (i0, vstep)));
		i0 = wasm_f32x4_add (i0, four);
	}
	if (i < count)
	{
		i0 = wasm_f32x4_add (wasm_f32x4_convert_i32x4 (Simd_W_Iota ()), wasm_f32x4_splat ((float)(count - 4)));
		wasm_v128_store (dest + count - 4, wasm_f32x4_add (vzi, wasm_f32x4_mul (i0, vstep)));
	}
}

/*
===============================================================================

PERSPECTIVE SPANS

===============================================================================
*/

// the perspective stepping of simd_scalar.c, four subdivisions at a time: the
// running sums of s/z, t/z and 1/z stay serial, the division and the rest are
// in lanes
typedef struct
{
	const simd_texmap_t	*map;
	float		sdivz, tdivz, zi;
	int			s, t;				// where the next subdivision starts
	int			count, shift;
	int			subdivisions;
} w_stepper_t;

// s and t at the start of each subdivision of a batch, and their steps
typedef struct
{
	int32_t		sstart[4], sstep[4], tstart[4], tstep[4];
} w_batch_t;

static int Simd_W_ClampScalar (int x, int lo, int hi)
{
	if (x > hi)
		return hi;
	if (x < lo)
		return lo;
	return x;
}

// hi if above it, else lo if below it
static inline v128_t Simd_W_Clamp (v128_t x, int lo, int hi)
{
	v128_t	vhi = wasm_i32x4_splat (hi);

	return wasm_v128_bitselect (vhi, wasm_i32x4_max (x, wasm_i32x4_splat (lo)), wasm_i32x4_gt (x, vhi));
}

SIMD_INLINE void Simd_W_SpanStart (w_stepper_t *st, const simd_texmap_t *map, int u, int v, int count, int shift)
{
	float	du = (float)u, dv = (float)v, z;

	st->map = map;
	st->count = count;
	st->shift = shift;
	st->subdivisions = (count + (1 << shift) - 1) >> shift;
	st->sdivz = map->sdivzorigin + dv*map->sdivzstepv + du*map->sdivzstepu;
	st->tdivz = map->tdivzorigin + dv*map->tdivzstepv + du*map->tdivzstepu;
	st->zi = map->ziorigin + dv*map->zistepv + du*map->zistepu;
	z = (float)0x10000 / st->zi;
	st->s = Simd_W_ClampScalar ((int)(st->sdivz * z) + map->sadjust, 0, map->sextent);
	st->t = Simd_W_ClampScalar ((int)(st->tdivz * z) + map->tadjust, 0, map->textent);
}

// subdivisions first .. first+n-1 (n at most 4), in lanes 0 .. n-1
SIMD_INLINE void Simd_W_SpanBatch (w_stepper_t *st, int first, int n, w_batch_t *b)
{
	const simd_texmap_t	*map = st->map;
	float		sd[4] = {0, 0, 0, 0}, td[4] = {0, 0, 0, 0}, zd[4] = {1, 1, 1, 1}, m1;
	int			k, len, step = 1 << st->shift, last = st->subdivisions - 1;
	v128_t		z, sn, tn, sstart, tstart;

	// where each subdivision ends; the last one ends on the span's last pixel
	for (k = 0 ; k < n ; k++)
	{
		if (first + k < last)
		{
			st->sdivz += map->sdivzstepu * (float)step;
			st->tdivz += map->tdivzstepu * (float)step;
			st->zi += map->zistepu * (float)step;
			sd[k] = st->sdivz;
			td[k] = st->tdivz;
			zd[k] = st->zi;
		}
		else
		{
			m1 = (float)(st->count - (last << st->shift) - 1);
			sd[k] = st->sdivz + map->sdivzstepu * m1;
			td[k] = st->tdivz + map->tdivzstepu * m1;
			zd[k] = st->zi + map->zistepu * m1;
		}
	}
	z = wasm_f32x4_div (wasm_f32x4_splat ((float)0x10000), wasm_v128_load (zd));
	sn = wasm_i32x4_add (wasm_i32x4_trunc_sat_f32x4 (wasm_f32x4_mul (wasm_v128_load (sd), z)),
		wasm_i32x4_splat (map->sadjust));
	tn = wasm_i32x4_add (wasm_i32x4_trunc_sat_f32x4 (wasm_f32x4_mul (wasm_v128_load (td), z)),
		wasm_i32x4_splat (map->tadjust));
	sn = Simd_W_Clamp (sn, step, map->sextent);
	tn = Simd_W_Clamp (tn, step, map->textent);

	// each subdivision starts where the one before ends
	sstart = wasm_i32x4_shuffle (wasm_i32x4_splat (st->s), sn, 3, 4, 5, 6);
	tstart = wasm_i32x4_shuffle (wasm_i32x4_splat (st->t), tn, 3, 4, 5, 6);
	wasm_v128_store (b->sstart, sstart);
	wasm_v128_store (b->tstart, tstart);
	wasm_v128_store (b->sstep, wasm_i32x4_shr (wasm_i32x4_sub (sn, sstart), st->shift));
	wasm_v128_store (b->tstep, wasm_i32x4_shr (wasm_i32x4_sub (tn, tstart), st->shift));

	// the last subdivision's steps by division
	if (first + n - 1 == last)
	{
		int32_t	s[4], t[4];

		k = n - 1;
		len = st->count - (last << st->shift);
		wasm_v128_store (s, sn);
		wasm_v128_store (t, tn);
		if (len > 1)
		{
			b->sstep[k] = (s[k] - b->sstart[k]) / (len - 1);
			b->tstep[k] = (t[k] - b->tstart[k]) / (len - 1);
		}
		st->s = s[k];
		st->t = t[k];
		return;
	}
	st->s = wasm_i32x4_extract_lane (sn, 3);
	st->t = wasm_i32x4_extract_lane (tn, 3);
}

// a pixel is a texel loaded and stored, which lanes don't speed: the scalar
// kernel, whose division for a subdivision overlaps the pixels of the one
// before, as on NEON
void Simd_W_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	Simd_Scalar_TexSpan (dest, map, src, srcwidth, u, v, count);
}

// spans of fewer than 32 pixels are the scalar kernel's, which steps one
// subdivision at a time faster than this does four, the division of each
// alongside the pixels of the one before; longer ones take the four's
// division in lanes ahead of the pixels. Both draw the same.
#define W_TURB_SCALAR	32

void Simd_W_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	w_stepper_t	st;
	w_batch_t	b;
	int			first, n, k, pixels, s, t, sstep, tstep, sturb, tturb;

	if (count < W_TURB_SCALAR)
	{
		Simd_Scalar_TurbSpan (dest, map, src, palette, turb, u, v, count);
		return;
	}
	Simd_W_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_W_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++)
		{
			pixels = count - ((first + k) << 4) < 16 ? count - ((first + k) << 4) : 16;
			s = b.sstart[k] & ((128 << 16) - 1);
			t = b.tstart[k] & ((128 << 16) - 1);
			sstep = b.sstep[k];
			tstep = b.tstep[k];
			for ( ; pixels > 0 ; pixels--, s += sstep, t += tstep)
			{
				sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
				tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
				*dest++ = palette[src[(tturb << 6) + sturb]];
			}
		}
	}
}

// Simd_W_TurbSpan's stepping, the texels read as they are
void Simd_W_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
	const int *turb, int u, int v, int count)
{
	w_stepper_t	st;
	w_batch_t	b;
	int			first, n, k, pixels, s, t, sstep, tstep, sturb, tturb;

	if (count < W_TURB_SCALAR)
	{
		Simd_Scalar_TurbSpanRGB30 (dest, map, src, turb, u, v, count);
		return;
	}
	Simd_W_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_W_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++)
		{
			pixels = count - ((first + k) << 4) < 16 ? count - ((first + k) << 4) : 16;
			s = b.sstart[k] & ((128 << 16) - 1);
			t = b.tstart[k] & ((128 << 16) - 1);
			sstep = b.sstep[k];
			tstep = b.tstep[k];
			for ( ; pixels > 0 ; pixels--, s += sstep, t += tstep)
			{
				sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
				tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
				*dest++ = src[(tturb << 6) + sturb];
			}
		}
	}
}

// four RGB30 texels of a lit liquid in their light, with their glow, as
// Simd_TurbLitTexel lights them
SIMD_INLINE v128_t Simd_W_TurbLit4 (v128_t p, v128_t g, v128_t l, v128_t gs)
{
	const v128_t	channel = wasm_u32x4_splat (1023);
	v128_t			out = wasm_u32x4_splat (0), c, f;
	int				k;

	for (k = 0 ; k < 3 ; k++)
	{
		c = wasm_i32x4_mul (wasm_v128_and (wasm_u32x4_shr (p, 10 * k), channel),
			wasm_v128_and (wasm_u32x4_shr (l, 10 * k), channel));
		f = wasm_i32x4_mul (wasm_v128_and (wasm_u32x4_shr (g, 10 * k), channel), gs);
		c = wasm_u32x4_min (wasm_u32x4_max (wasm_u32x4_shr (c, 9), wasm_u32x4_shr (f, 15)), channel);
		out = wasm_v128_or (out, wasm_i32x4_shl (c, 10 * k));
	}
	return out;
}

// a pixel of a lit liquid: its texel in the 64x64 texture, and its light
SIMD_INLINE int Simd_W_TurbLitAt (const simd_turblight_t *lit, const int *turb, int shift, int s, int t,
	int ls, int lt, uint32_t *light)
{
	int		sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
	int		tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;

	*light = lit->light[(lt >> shift) * lit->width + (ls >> shift)];
	return (tturb << 6) + sturb;
}

// Simd_W_TurbSpan's stepping, the light read where s and t are; in RGB, four
// pixels at a time: their texels and light looked up as four chains of their
// own, put in lanes, and lit in the lanes. Through the colormap, spans of
// fewer than 32 pixels are the scalar kernel's, as Simd_W_TurbSpan's are; in
// RGB, of fewer than 8.
#define W_TURBLIT_SCALAR	8

void Simd_W_TurbSpanLit (uint32_t *dest, const simd_texmap_t *map, const simd_turblight_t *lit,
	const int *turb, int u, int v, int count)
{
	const v128_t	gs = wasm_u32x4_splat ((uint32_t)lit->glowscale);
	const uint32_t	*texels = lit->texels, *glow = lit->glow;
	w_stepper_t		st;
	w_batch_t		b;
	uint32_t		l0, l1, l2, l3;
	v128_t			g;
	int				first, n, k, pixels, s, t, ls, lt, sstep, tstep, x0, x1, x2, x3, shift = 16 + lit->shift;

	if (count < (lit->colormap ? W_TURB_SCALAR : W_TURBLIT_SCALAR))
	{
		Simd_Scalar_TurbSpanLit (dest, map, lit, turb, u, v, count);
		return;
	}
	Simd_W_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_W_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++)
		{
			pixels = count - ((first + k) << 4) < 16 ? count - ((first + k) << 4) : 16;
			ls = b.sstart[k];
			lt = b.tstart[k];
			s = (ls + lit->soffset) & ((128 << 16) - 1);
			t = (lt + lit->toffset) & ((128 << 16) - 1);
			sstep = b.sstep[k];
			tstep = b.tstep[k];
			for ( ; pixels >= 4 && !lit->colormap ; pixels -= 4, dest += 4)
			{
				x0 = Simd_W_TurbLitAt (lit, turb, shift, s, t, ls, lt, &l0);
				x1 = Simd_W_TurbLitAt (lit, turb, shift, s + sstep, t + tstep, ls + sstep, lt + tstep, &l1);
				x2 = Simd_W_TurbLitAt (lit, turb, shift, s + 2 * sstep, t + 2 * tstep, ls + 2 * sstep,
					lt + 2 * tstep, &l2);
				x3 = Simd_W_TurbLitAt (lit, turb, shift, s + 3 * sstep, t + 3 * tstep, ls + 3 * sstep,
					lt + 3 * tstep, &l3);
				g = glow ? wasm_u32x4_make (glow[x0], glow[x1], glow[x2], glow[x3]) : wasm_u32x4_splat (0);
				wasm_v128_store (dest, Simd_W_TurbLit4 (wasm_u32x4_make (texels[x0], texels[x1], texels[x2],
					texels[x3]), g, wasm_u32x4_make (l0, l1, l2, l3), gs));
				s += 4 * sstep;
				t += 4 * tstep;
				ls += 4 * sstep;
				lt += 4 * tstep;
			}
			for ( ; pixels > 0 ; pixels--, s += sstep, t += tstep, ls += sstep, lt += tstep)
			{
				x0 = Simd_W_TurbLitAt (lit, turb, shift, s, t, ls, lt, &l0);
				*dest++ = Simd_TurbLitTexel (lit, x0, l0);
			}
		}
	}
}

/*
===============================================================================

LIGHTING

===============================================================================
*/

// the lookups are all of it, as on NEON: the scalar kernel
void Simd_W_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	Simd_Scalar_LitRowColormap (dest, src, colormap, light, step, count);
}

// two lookups a texel are most of it, as on NEON: the scalar kernel
void Simd_W_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	Simd_Scalar_LitRowRGB (dest, src, palette, floor, light, step, count);
}

// channel k of 4 colors lit (light 32768 being 1.0), at least the glow's
// channel times glowscale, at most 1023
static inline v128_t Simd_W_GlowChannel (v128_t out, v128_t pix, v128_t glow, v128_t gs, v128_t l, int k)
{
	const v128_t	channel = wasm_u32x4_splat (1023);
	v128_t			c, f;

	c = wasm_u32x4_shr (wasm_i32x4_mul (wasm_v128_and (wasm_u32x4_shr (pix, 10 * k), channel), l), 15);
	f = wasm_u32x4_shr (wasm_i32x4_mul (wasm_v128_and (wasm_u32x4_shr (glow, 10 * k), channel), gs), 15);
	c = wasm_u32x4_min (wasm_u32x4_max (c, f), channel);
	return wasm_v128_or (out, wasm_i32x4_shl (c, 10 * k));
}

void Simd_W_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
	const int light[3], const int step[3], int count)
{
	const v128_t	gs = wasm_u32x4_splat ((uint32_t)glowscale);
	v128_t			l[3], st[3], rev, pix, gl, out;
	int				j, k;

	if (count < 4)
	{
		Simd_Scalar_LitRowRGB30 (dest, src, glow, glowscale, light, step, count);
		return;
	}
	// the light in registers: the stores to dest could be to it, as far as the
	// compiler knows, and it would be read again every four texels
	for (k = 0 ; k < 3 ; k++)
	{
		l[k] = wasm_i32x4_splat (light[k]);
		st[k] = wasm_i32x4_splat (step[k]);
	}
	rev = wasm_i32x4_sub (wasm_i32x4_splat (count - 1), Simd_W_Iota ());
	for (j = 0 ; j + 4 <= count ; j += 4, rev = wasm_i32x4_sub (rev, wasm_i32x4_splat (4)))
	{
		pix = wasm_v128_load (src + j);
		gl = glow ? wasm_v128_load (glow + j) : wasm_u32x4_splat (0);
		out = wasm_u32x4_splat (0);
		for (k = 0 ; k < 3 ; k++)
			out = Simd_W_GlowChannel (out, pix, gl, gs,
				wasm_i32x4_max (wasm_i32x4_add (l[k], wasm_i32x4_mul (rev, st[k])), wasm_i32x4_splat (0)), k);
		wasm_v128_store (dest + j, out);
	}
	// the last few: a texel's light goes by how far it is from the row's end,
	// so they are lit the same as a row of their own
	if (j < count)
		Simd_Scalar_LitRowRGB30 (dest + j, src + j, glow ? glow + j : NULL, glowscale, light, step, count - j);
}

/*
===============================================================================

MODELS

===============================================================================
*/

// channel k of 4 colors lit (light 32768 being 1.0), at least the floor's, at
// most 1023, in place
static inline v128_t Simd_W_LitChannel (v128_t out, v128_t pal, v128_t fl, v128_t l, int k)
{
	const v128_t	channel = wasm_u32x4_splat (1023);
	v128_t			c, f;

	c = wasm_v128_and (wasm_u32x4_shr (pal, 10 * k), channel);
	c = wasm_u32x4_shr (wasm_i32x4_mul (c, l), 15);
	f = wasm_v128_and (wasm_u32x4_shr (fl, 10 * k), channel);
	c = wasm_u32x4_min (wasm_u32x4_max (c, f), channel);
	return wasm_v128_or (out, wasm_i32x4_shl (c, 10 * k));
}

// four pixels at a time, the skin's, remap's, palette's and floor's entries
// loaded into the lanes and the pixels in front blended into the row and the z
// buffer whole; the last few (a span of fewer than four is all of them) the
// scalar kernel's, which steps on from where the four left off as they did
void Simd_W_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	const v128_t	lane = Simd_W_Iota ();
	const v128_t	zistep = wasm_i32x4_mul (lane, wasm_i32x4_splat (map->zistep));
	const v128_t	lightstep = wasm_i32x4_mul (lane, wasm_i32x4_splat (map->lightstep));
	const v128_t	sstep = wasm_i32x4_mul (lane, wasm_i32x4_splat (map->sfracstep));
	const v128_t	tstep = wasm_i32x4_mul (lane, wasm_i32x4_splat (map->tfracstep));
	const v128_t	whole = wasm_i32x4_mul (lane, wasm_i32x4_splat (map->stepwhole));
	const v128_t	skinwidth = wasm_i32x4_splat (map->skinwidth);
	const v128_t	full = wasm_i32x4_splat (255 << 6);
	const v128_t	hole = wasm_u32x4_splat (255);
	v128_t			z, zb, visible, texel, color, pal, fl, level, s, t, l;
	int				i, k, snext, tnext;

	pal = fl = wasm_u32x4_splat (0);
	for (i = 0 ; i + 4 <= count ; i += 4)
	{
		z = wasm_f32x4_mul (wasm_f32x4_convert_i32x4 (wasm_i32x4_add (wasm_i32x4_splat (zi), zistep)),
			wasm_f32x4_splat (1.0f / 2147483648.0f));
		zb = wasm_v128_load (zbuf + i);
		visible = wasm_f32x4_ge (z, zb);
		if (wasm_v128_any_true (visible))
		{
			// each pixel's texel: the whole steps and the carries of both fractions
			// (whole steps back are behind tex); the four lookups each pixel
			// takes, the skin's, remap's and then palette's and floor's, are
			// four chains of their own, put together in lanes after
			s = wasm_i32x4_add (wasm_i32x4_splat (sfrac), sstep);
			t = wasm_i32x4_add (wasm_i32x4_splat (tfrac), tstep);
			{
				v128_t		at = wasm_i32x4_add (whole, wasm_i32x4_add (wasm_i32x4_shr (s, 16),
					wasm_i32x4_mul (wasm_i32x4_shr (t, 16), skinwidth)));
				uint32_t	t0 = tex[wasm_i32x4_extract_lane (at, 0)], t1 = tex[wasm_i32x4_extract_lane (at, 1)];
				uint32_t	t2 = tex[wasm_i32x4_extract_lane (at, 2)], t3 = tex[wasm_i32x4_extract_lane (at, 3)];
				uint32_t	r0 = map->remap[t0], r1 = map->remap[t1], r2 = map->remap[t2], r3 = map->remap[t3];

				texel = wasm_u32x4_make (t0, t1, t2, t3);
				if (map->colormap)
				{
					// each pixel's light, as the lanes have it: four steps a group
					const uint32_t	*cm = map->colormap;
					unsigned		ul = (unsigned)light, us = (unsigned)map->lightstep;

					color = wasm_u32x4_make (cm[r0 + (ul & 0xFF00)], cm[r1 + ((ul + us) & 0xFF00)],
						cm[r2 + ((ul + 2 * us) & 0xFF00)], cm[r3 + ((ul + 3 * us) & 0xFF00)]);
				}
				else
				{
					pal = wasm_u32x4_make (map->palette[r0], map->palette[r1], map->palette[r2], map->palette[r3]);
					fl = wasm_u32x4_make (map->floor[r0], map->floor[r1], map->floor[r2], map->floor[r3]);
				}
			}
			if (map->holey)
				visible = wasm_v128_andnot (visible, wasm_i32x4_eq (texel, hole));
			if (!map->colormap)
			{
				l = wasm_i32x4_add (wasm_i32x4_splat (light), lightstep);
				level = wasm_v128_andnot (wasm_i32x4_sub (full, l), wasm_i32x4_ge (l, full));
				color = wasm_u32x4_splat (0);
				for (k = 0 ; k < 3 ; k++)
					color = Simd_W_LitChannel (color, pal, fl,
						wasm_u32x4_shr (wasm_i32x4_mul (level, wasm_u32x4_splat (map->tint[k])), 6), k);
			}
			wasm_v128_store (dest + i, wasm_v128_bitselect (color, wasm_v128_load (dest + i), visible));
			wasm_v128_store (zbuf + i, wasm_v128_bitselect (z, zb, visible));
		}

		// four pixels on
		zi = (int)((unsigned)zi + 4u * (unsigned)map->zistep);
		light = (int)((unsigned)light + 4u * (unsigned)map->lightstep);
		snext = sfrac + 4 * map->sfracstep;
		tnext = tfrac + 4 * map->tfracstep;
		tex += 4 * map->stepwhole + (snext >> 16) + (tnext >> 16) * map->skinwidth;
		sfrac = snext & 0xFFFF;
		tfrac = tnext & 0xFFFF;
	}
	if (i < count)
		Simd_Scalar_AliasSpan (dest + i, zbuf + i, tex, sfrac, tfrac, light, zi, count - i, map);
}

// the first n (1 to 3) of four words in lanes, the rest 0: a span's last few,
// where a whole vector would read past it
SIMD_INLINE v128_t Simd_W_LoadFew (const void *p, int n)
{
	v128_t	v = wasm_v128_load32_zero (p);

	if (n > 1)
		v = wasm_v128_load32_lane ((const byte *)p + 4, v, 1);
	if (n > 2)
		v = wasm_v128_load32_lane ((const byte *)p + 8, v, 2);
	return v;
}

// the first n (1 to 3) lanes stored
SIMD_INLINE void Simd_W_StoreFew (void *p, v128_t v, int n)
{
	wasm_v128_store32_lane (p, v, 0);
	if (n > 1)
		wasm_v128_store32_lane ((byte *)p + 4, v, 1);
	if (n > 2)
		wasm_v128_store32_lane ((byte *)p + 8, v, 2);
}

/*
===============================================================================

BLENDING AND 2D

===============================================================================
*/

// one 10 bit channel of src over dest at bit shift, in linear light as
// simd_scalar.c blends it
static inline v128_t Simd_W_BlendChannel (v128_t s, v128_t d, v128_t a, v128_t ia, int shift)
{
	const v128_t	mask = wasm_u32x4_splat (1023);
	v128_t			fs = wasm_f32x4_convert_u32x4 (wasm_v128_and (wasm_u32x4_shr (s, shift), mask));
	v128_t			fd = wasm_f32x4_convert_u32x4 (wasm_v128_and (wasm_u32x4_shr (d, shift), mask));
	v128_t			x, c;

	fs = wasm_f32x4_mul (fs, fs);
	fs = wasm_f32x4_mul (fs, fs);
	fd = wasm_f32x4_mul (fd, fd);
	fd = wasm_f32x4_mul (fd, fd);
	x = wasm_f32x4_mul (wasm_f32x4_add (wasm_f32x4_mul (fs, a), wasm_f32x4_mul (fd, ia)),
		wasm_f32x4_splat (1.0f / 256.0f));
	c = wasm_u32x4_trunc_sat_f32x4 (wasm_f32x4_add (wasm_f32x4_sqrt (wasm_f32x4_sqrt (x)), wasm_f32x4_splat (0.5f)));
	return wasm_i32x4_shl (wasm_u32x4_min (c, mask), shift);
}

// four pixels; lanes whose mask is clear keep dest's
static inline v128_t Simd_W_Blend4 (v128_t s, v128_t d, v128_t m, v128_t a, v128_t ia)
{
	v128_t	out = wasm_v128_or (Simd_W_BlendChannel (s, d, a, ia, 0),
		wasm_v128_or (Simd_W_BlendChannel (s, d, a, ia, 10), Simd_W_BlendChannel (s, d, a, ia, 20)));

	return wasm_v128_bitselect (out, d, m);
}

void Simd_W_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
	int alpha, int count)
{
	const v128_t	vzi = wasm_f32x4_splat (zi), vstep = wasm_f32x4_splat (step);
	const v128_t	a = wasm_f32x4_splat ((float)alpha), ia = wasm_f32x4_splat ((float)(256 - alpha));
	const v128_t	cutout = wasm_u32x4_splat (0x80000000u);
	v128_t			idx = Simd_W_Iota (), s, m;
	int				i, n;

	for (i = 0 ; i < count ; i += 4, idx = wasm_i32x4_add (idx, wasm_i32x4_splat (4)))
	{
		// the last few a lane at a time; the lanes past them are stored nowhere
		n = count - i < 4 ? count - i : 4;
		s = n == 4 ? wasm_v128_load (src + i) : Simd_W_LoadFew (src + i, n);
		m = wasm_i32x4_eq (wasm_v128_and (s, cutout), wasm_u32x4_splat (0));
		if (zbuf)
			m = wasm_v128_and (m, wasm_f32x4_le (n == 4 ? wasm_v128_load (zbuf + i) : Simd_W_LoadFew (zbuf + i, n),
				wasm_f32x4_add (vzi, wasm_f32x4_mul (wasm_f32x4_convert_i32x4 (idx), vstep))));
		if (!wasm_v128_any_true (m))
			continue;
		if (n == 4)
			wasm_v128_store (dest + i, Simd_W_Blend4 (s, wasm_v128_load (dest + i), m, a, ia));
		else
			Simd_W_StoreFew (dest + i, Simd_W_Blend4 (s, Simd_W_LoadFew (dest + i, n), m, a, ia), n);
	}
}

// one 10 bit channel of dest at bit shift: its light times m, plus s, as
// simd_scalar.c adds it (pmax: 0 for a sum of -0 or less, as the scalar's x > 0)
static inline v128_t Simd_W_PartChannel (v128_t d, v128_t s, v128_t m, int shift)
{
	const v128_t	mask = wasm_u32x4_splat (1023);
	v128_t			fd = wasm_f32x4_convert_u32x4 (wasm_v128_and (wasm_u32x4_shr (d, shift), mask));
	v128_t			x, c;

	fd = wasm_f32x4_mul (fd, fd);
	fd = wasm_f32x4_mul (fd, fd);
	x = wasm_f32x4_pmax (wasm_f32x4_splat (0), wasm_f32x4_add (s, wasm_f32x4_mul (fd, m)));
	c = wasm_u32x4_trunc_sat_f32x4 (wasm_f32x4_add (wasm_f32x4_sqrt (wasm_f32x4_sqrt (x)), wasm_f32x4_splat (0.5f)));
	return wasm_i32x4_shl (wasm_u32x4_min (c, mask), shift);
}

void Simd_W_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
	const float *const src[3], const float *const mul[3], int count)
{
	const v128_t	vzi = wasm_f32x4_splat (zi), vstep = wasm_f32x4_splat (step);
	v128_t			idx = Simd_W_Iota (), sv[3], mv[3], d, m, out;
	int				i, c, n;

	for (i = 0 ; i < count ; i += 4, idx = wasm_i32x4_add (idx, wasm_i32x4_splat (4)))
	{
		// the last few a lane at a time; the lanes past them are stored nowhere
		n = count - i < 4 ? count - i : 4;
		m = wasm_u32x4_splat (0xFFFFFFFFu);
		if (zbuf)
			m = wasm_f32x4_le (n == 4 ? wasm_v128_load (zbuf + i) : Simd_W_LoadFew (zbuf + i, n),
				wasm_f32x4_add (vzi, wasm_f32x4_mul (wasm_f32x4_convert_i32x4 (idx), vstep)));
		if (!wasm_v128_any_true (m))
			continue;
		for (c = 0 ; c < 3 ; c++)
		{
			sv[c] = n == 4 ? wasm_v128_load (src[c] + i) : Simd_W_LoadFew (src[c] + i, n);
			mv[c] = n == 4 ? wasm_v128_load (mul[c] + i) : Simd_W_LoadFew (mul[c] + i, n);
		}
		d = n == 4 ? wasm_v128_load (dest + i) : Simd_W_LoadFew (dest + i, n);
		out = wasm_v128_or (Simd_W_PartChannel (d, sv[0], mv[0], 0),
			wasm_v128_or (Simd_W_PartChannel (d, sv[1], mv[1], 10), Simd_W_PartChannel (d, sv[2], mv[2], 20)));
		out = wasm_v128_bitselect (out, d, m);
		if (n == 4)
			wasm_v128_store (dest + i, out);
		else
			Simd_W_StoreFew (dest + i, out, n);
	}
}

// one 10 bit channel at bit shift fogged, as simd_scalar.c fogs it
static inline v128_t Simd_W_FogChannel (v128_t p, v128_t fog, v128_t a, v128_t ia, int shift)
{
	const v128_t	mask = wasm_u32x4_splat (1023);
	v128_t			fs = wasm_f32x4_convert_u32x4 (wasm_v128_and (wasm_u32x4_shr (p, shift), mask));
	v128_t			x, c;

	fs = wasm_f32x4_mul (fs, fs);
	fs = wasm_f32x4_mul (fs, fs);
	x = wasm_f32x4_mul (wasm_f32x4_add (wasm_f32x4_mul (fs, a), wasm_f32x4_mul (fog, ia)),
		wasm_f32x4_splat (1.0f / 256.0f));
	c = wasm_u32x4_trunc_sat_f32x4 (wasm_f32x4_add (wasm_f32x4_sqrt (wasm_f32x4_sqrt (x)), wasm_f32x4_splat (0.5f)));
	return wasm_i32x4_shl (wasm_u32x4_min (c, mask), shift);
}

// four pixels fogged; the lanes whose top bit is set are left as they are
static inline v128_t Simd_W_Fog4 (v128_t p, v128_t z, const simd_fog_t *fog)
{
	const v128_t	base = wasm_i32x4_splat (fog->base), last = wasm_i32x4_splat (fog->size - 1);
	const v128_t	full = wasm_f32x4_splat (256.0f);
	const v128_t	zero = wasm_i32x4_splat (0);
	const uint32_t	*bits = (const uint32_t *)fog->table;	// strict aliasing is off
	v128_t			e, a, ia, out;

	// the table's entries by the bits of 1/z, negative ones the first
	e = wasm_i32x4_sub (wasm_i32x4_shr (z, SIMD_FOG_SHIFT), base);
	e = wasm_i32x4_min (wasm_i32x4_max (e, zero), last);
	a = wasm_u32x4_make (bits[wasm_i32x4_extract_lane (e, 0)], bits[wasm_i32x4_extract_lane (e, 1)],
		bits[wasm_i32x4_extract_lane (e, 2)], bits[wasm_i32x4_extract_lane (e, 3)]);
	a = wasm_v128_bitselect (wasm_f32x4_splat (fog->sky), a,
		wasm_i32x4_ne (wasm_v128_and (p, wasm_u32x4_splat (0x40000000u)), zero));
	ia = wasm_f32x4_sub (full, a);

	out = wasm_v128_or (Simd_W_FogChannel (p, wasm_f32x4_splat (fog->color[0]), a, ia, 0),
		wasm_v128_or (Simd_W_FogChannel (p, wasm_f32x4_splat (fog->color[1]), a, ia, 10),
			Simd_W_FogChannel (p, wasm_f32x4_splat (fog->color[2]), a, ia, 20)));
	return wasm_v128_bitselect (p, out, wasm_i32x4_ne (wasm_v128_and (p, wasm_u32x4_splat (0x80000000u)), zero));
}

void Simd_W_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
	const simd_fog_t *fog)
{
	const v128_t	vzi = wasm_f32x4_splat (zi), vstep = wasm_f32x4_splat (step);
	v128_t			idx = Simd_W_Iota (), z;
	int				i, n;

	for (i = 0 ; i + 4 <= count ; i += 4, idx = wasm_i32x4_add (idx, wasm_i32x4_splat (4)))
	{
		z = zbuf ? wasm_v128_load (zbuf + i) : wasm_f32x4_add (vzi, wasm_f32x4_mul (wasm_f32x4_convert_i32x4 (idx), vstep));
		wasm_v128_store (dest + i, Simd_W_Fog4 (wasm_v128_load (dest + i), z, fog));
	}
	// the last few a lane at a time; the lanes past them are stored nowhere
	if (i < count)
	{
		n = count - i;
		z = zbuf ? Simd_W_LoadFew (zbuf + i, n) : wasm_f32x4_add (vzi, wasm_f32x4_mul (wasm_f32x4_convert_i32x4 (idx), vstep));
		Simd_W_StoreFew (dest + i, Simd_W_Fog4 (Simd_W_LoadFew (dest + i, n), z, fog), n);
	}
}

// the bytes of a vector of four colors that make each of its vectors with
// every color repeated scale times, for wasm_i8x16_swizzle
static const uint8_t	expand_tbl[5][4][16] = {
	[1] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
	[2] = {{0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 6, 7},
		{8, 9, 10, 11, 8, 9, 10, 11, 12, 13, 14, 15, 12, 13, 14, 15}},
	[3] = {{0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7},
		{4, 5, 6, 7, 4, 5, 6, 7, 8, 9, 10, 11, 8, 9, 10, 11},
		{8, 9, 10, 11, 12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15}},
	[4] = {{0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3},
		{4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7},
		{8, 9, 10, 11, 8, 9, 10, 11, 8, 9, 10, 11, 8, 9, 10, 11},
		{12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15}},
};

// four texels at a time, with scale a constant where it is inlined: their
// colors repeated in place and stored whole, or blended with what is there
// around transparent ones
SIMD_INLINE void Simd_W_Expand8Scaled (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	const int scale, int transparent)
{
	v128_t		tbl[4], colors, skip, o;
	uint32_t	*d;
	int			i, q, k;

	for (q = 0 ; q < scale ; q++)
		tbl[q] = wasm_v128_load (expand_tbl[scale][q]);
	for (i = 0 ; i + 4 <= count ; i += 4)
	{
		d = dest + (size_t)i * scale;
		colors = Simd_W_LookupBytes (palette, src + i);
		if (transparent < 0)
		{
			for (q = 0 ; q < scale ; q++)
				wasm_v128_store (d + 4 * q, scale == 1 ? colors : wasm_i8x16_swizzle (colors, tbl[q]));
			continue;
		}
		skip = wasm_i32x4_eq (Simd_W_Widen (src + i), wasm_u32x4_splat ((uint32_t)transparent));
		if (wasm_i32x4_all_true (skip))
			continue;		// all four skipped
		if (!wasm_v128_any_true (skip))
		{
			for (q = 0 ; q < scale ; q++)
				wasm_v128_store (d + 4 * q, scale == 1 ? colors : wasm_i8x16_swizzle (colors, tbl[q]));
			continue;
		}
		for (q = 0 ; q < scale ; q++)
		{
			o = wasm_v128_load (d + 4 * q);
			if (scale == 1)
				o = wasm_v128_bitselect (o, colors, skip);
			else
				o = wasm_v128_bitselect (o, wasm_i8x16_swizzle (colors, tbl[q]), wasm_i8x16_swizzle (skip, tbl[q]));
			wasm_v128_store (d + 4 * q, o);
		}
	}
	for ( ; i < count ; i++)
	{
		if (src[i] == transparent)
			continue;
		d = dest + (size_t)i * scale;
		for (k = 0 ; k < scale ; k++)
			d[k] = palette[src[i]];
	}
}

// scales 1 to 4; at more, each texel is a whole vector or more of one color,
// and the compiler vectorizes the scalar kernel's fills as well as this could
void Simd_W_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	switch (scale)
	{
	case 1:
		Simd_W_Expand8Scaled (dest, src, palette, count, 1, transparent);
		break;
	case 2:
		Simd_W_Expand8Scaled (dest, src, palette, count, 2, transparent);
		break;
	case 3:
		Simd_W_Expand8Scaled (dest, src, palette, count, 3, transparent);
		break;
	case 4:
		Simd_W_Expand8Scaled (dest, src, palette, count, 4, transparent);
		break;
	default:
		Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
		break;
	}
}

// libc's copy is WebAssembly's memory.copy
void Simd_W_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	Simd_Scalar_CopyStream (dest, destpitch, src, srcpitch, rowbytes, rows);
}
