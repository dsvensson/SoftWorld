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
// simd_apple_m3.c -- the kernels for Apple M3 and later (-mcpu=apple-m3): NEON,
// four 32 bit lanes. Same results as simd_scalar.c, bit for bit: multiplies and
// adds apart (never fused), square roots and divisions correctly rounded, and
// float to int as the scalar casts do it (the same instructions on arm64).
//
// NEON has no gather: what is looked up in a table is loaded a lane at a time,
// and the arithmetic around it is in lanes. Where that doesn't beat the scalar
// kernel as the compiler builds it, the scalar kernel is used.

#include "simd_backends.h"

#include <arm_neon.h>
#include <string.h>

static inline int32x4_t Simd_M3_Iota (void)
{
	static const int32_t	iota[4] = {0, 1, 2, 3};

	return vld1q_s32 (iota);
}

// four table entries, by four bytes
SIMD_INLINE uint32x4_t Simd_M3_LookupBytes (const uint32_t *table, const byte *idx)
{
	uint32x4_t	v = vld1q_dup_u32 (table + idx[0]);

	v = vld1q_lane_u32 (table + idx[1], v, 1);
	v = vld1q_lane_u32 (table + idx[2], v, 2);
	return vld1q_lane_u32 (table + idx[3], v, 3);
}

// four bytes, zero extended to 32 bit lanes
SIMD_INLINE uint32x4_t Simd_M3_Widen (const byte *src)
{
	uint32_t	t;

	memcpy (&t, src, 4);
	return vmovl_u16 (vget_low_u16 (vmovl_u8 (vreinterpret_u8_u32 (vdup_n_u32 (t)))));
}

// sixteen at a time from four running indices, then four, then the last four
// again ending on the last pixel (the same values where they overlap)
void Simd_M3_ZSpan (float *dest, int count, float zi, float step)
{
	const float32x4_t	vzi = vdupq_n_f32 (zi), vstep = vdupq_n_f32 (step);
	const float32x4_t	four = vdupq_n_f32 (4), sixteen = vdupq_n_f32 (16);
	float32x4_t			i0, i1, i2, i3;		// whole numbers, exact
	int					i;

	if (count < 4)
	{
		for (i = 0 ; i < count ; i++)
			dest[i] = zi + (float)i * step;
		return;
	}
	i0 = vcvtq_f32_s32 (Simd_M3_Iota ());
	i1 = vaddq_f32 (i0, four);
	i2 = vaddq_f32 (i1, four);
	i3 = vaddq_f32 (i2, four);
	for (i = 0 ; i + 16 <= count ; i += 16)
	{
		vst1q_f32 (dest + i, vaddq_f32 (vzi, vmulq_f32 (i0, vstep)));
		vst1q_f32 (dest + i + 4, vaddq_f32 (vzi, vmulq_f32 (i1, vstep)));
		vst1q_f32 (dest + i + 8, vaddq_f32 (vzi, vmulq_f32 (i2, vstep)));
		vst1q_f32 (dest + i + 12, vaddq_f32 (vzi, vmulq_f32 (i3, vstep)));
		i0 = vaddq_f32 (i0, sixteen);
		i1 = vaddq_f32 (i1, sixteen);
		i2 = vaddq_f32 (i2, sixteen);
		i3 = vaddq_f32 (i3, sixteen);
	}
	for ( ; i + 4 <= count ; i += 4)
	{
		vst1q_f32 (dest + i, vaddq_f32 (vzi, vmulq_f32 (i0, vstep)));
		i0 = vaddq_f32 (i0, four);
	}
	if (i < count)
	{
		i0 = vaddq_f32 (vcvtq_f32_s32 (Simd_M3_Iota ()), vdupq_n_f32 ((float)(count - 4)));
		vst1q_f32 (dest + count - 4, vaddq_f32 (vzi, vmulq_f32 (i0, vstep)));
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
} m3_stepper_t;

// s and t at the start of each subdivision of a batch, and their steps
typedef struct
{
	int32_t		sstart[4], sstep[4], tstart[4], tstep[4];
} m3_batch_t;

static int Simd_M3_ClampScalar (int x, int lo, int hi)
{
	if (x > hi)
		return hi;
	if (x < lo)
		return lo;
	return x;
}

// hi if above it, else lo if below it
static inline int32x4_t Simd_M3_Clamp (int32x4_t x, int lo, int hi)
{
	int32x4_t	vhi = vdupq_n_s32 (hi);

	return vbslq_s32 (vcgtq_s32 (x, vhi), vhi, vmaxq_s32 (x, vdupq_n_s32 (lo)));
}

SIMD_INLINE void Simd_M3_SpanStart (m3_stepper_t *st, const simd_texmap_t *map, int u, int v, int count, int shift)
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
	st->s = Simd_M3_ClampScalar ((int)(st->sdivz * z) + map->sadjust, 0, map->sextent);
	st->t = Simd_M3_ClampScalar ((int)(st->tdivz * z) + map->tadjust, 0, map->textent);
}

// subdivisions first .. first+n-1 (n at most 4), in lanes 0 .. n-1
SIMD_INLINE void Simd_M3_SpanBatch (m3_stepper_t *st, int first, int n, m3_batch_t *b)
{
	const simd_texmap_t	*map = st->map;
	float		sd[4] = {0, 0, 0, 0}, td[4] = {0, 0, 0, 0}, zd[4] = {1, 1, 1, 1}, m1;
	int			k, len, step = 1 << st->shift, last = st->subdivisions - 1;
	float32x4_t	z;
	int32x4_t	sn, tn, sstart, tstart, shift = vdupq_n_s32 (-st->shift);

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
	z = vdivq_f32 (vdupq_n_f32 ((float)0x10000), vld1q_f32 (zd));
	sn = vaddq_s32 (vcvtq_s32_f32 (vmulq_f32 (vld1q_f32 (sd), z)), vdupq_n_s32 (map->sadjust));
	tn = vaddq_s32 (vcvtq_s32_f32 (vmulq_f32 (vld1q_f32 (td), z)), vdupq_n_s32 (map->tadjust));
	sn = Simd_M3_Clamp (sn, step, map->sextent);
	tn = Simd_M3_Clamp (tn, step, map->textent);

	// each subdivision starts where the one before ends
	sstart = vextq_s32 (vdupq_n_s32 (st->s), sn, 3);
	tstart = vextq_s32 (vdupq_n_s32 (st->t), tn, 3);
	vst1q_s32 (b->sstart, sstart);
	vst1q_s32 (b->tstart, tstart);
	vst1q_s32 (b->sstep, vshlq_s32 (vsubq_s32 (sn, sstart), shift));
	vst1q_s32 (b->tstep, vshlq_s32 (vsubq_s32 (tn, tstart), shift));

	// the last subdivision's steps by division
	if (first + n - 1 == last)
	{
		int32_t	s[4], t[4];

		k = n - 1;
		len = st->count - (last << st->shift);
		vst1q_s32 (s, sn);
		vst1q_s32 (t, tn);
		if (len > 1)
		{
			b->sstep[k] = (s[k] - b->sstart[k]) / (len - 1);
			b->tstep[k] = (t[k] - b->tstart[k]) / (len - 1);
		}
		st->s = s[k];
		st->t = t[k];
		return;
	}
	st->s = vgetq_lane_s32 (sn, 3);
	st->t = vgetq_lane_s32 (tn, 3);
}

// the scalar kernel, built with the rest, ran half as fast again as this did
// with its stepping four subdivisions at a time in lanes: a pixel is a texel
// loaded and stored, which lanes don't speed, and the scalar stepping's
// division for a subdivision overlaps the pixels of the one before
void Simd_M3_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	Simd_Scalar_TexSpan (dest, map, src, srcwidth, u, v, count);
}

void Simd_M3_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	m3_stepper_t	st;
	m3_batch_t		b;
	int				first, n, k, pixels, s, t, sstep, tstep, sturb, tturb;

	Simd_M3_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_M3_SpanBatch (&st, first, n, &b);
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

// Simd_M3_TurbSpan's stepping, the texels read as they are
void Simd_M3_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
	const int *turb, int u, int v, int count)
{
	m3_stepper_t	st;
	m3_batch_t		b;
	int				first, n, k, pixels, s, t, sstep, tstep, sturb, tturb;

	Simd_M3_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_M3_SpanBatch (&st, first, n, &b);
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

// Simd_M3_TurbSpan's stepping, the light read where s and t are
void Simd_M3_TurbSpanLit (uint32_t *dest, const simd_texmap_t *map, const simd_turblight_t *lit,
	const int *turb, int u, int v, int count)
{
	m3_stepper_t	st;
	m3_batch_t		b;
	int				first, n, k, pixels, s, t, ls, lt, sstep, tstep, sturb, tturb, shift = 16 + lit->shift;

	Simd_M3_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_M3_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++)
		{
			pixels = count - ((first + k) << 4) < 16 ? count - ((first + k) << 4) : 16;
			ls = b.sstart[k];
			lt = b.tstart[k];
			s = (ls + lit->soffset) & ((128 << 16) - 1);
			t = (lt + lit->toffset) & ((128 << 16) - 1);
			sstep = b.sstep[k];
			tstep = b.tstep[k];
			for ( ; pixels > 0 ; pixels--, s += sstep, t += tstep, ls += sstep, lt += tstep)
			{
				sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
				tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
				*dest++ = Simd_TurbLitTexel (lit, (tturb << 6) + sturb,
					lit->light[(lt >> shift) * lit->width + (ls >> shift)]);
			}
		}
	}
}

/*
===============================================================================

LIGHTING

===============================================================================
*/

// the lookups are all of it: with the index arithmetic in lanes and the
// entries loaded straight into them, it ran no faster than the scalar kernel
// built with the rest (link-time optimization), and rows of two ran slower
void Simd_M3_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	Simd_Scalar_LitRowColormap (dest, src, colormap, light, step, count);
}

// channel k of 4 colors lit (light 32768 being 1.0), at least the floor's, at
// most 1023, in place
static inline uint32x4_t Simd_M3_LitChannel (uint32x4_t out, uint32x4_t pal, uint32x4_t fl, uint32x4_t l,
	int k)
{
	const uint32x4_t	channel = vdupq_n_u32 (1023);
	const int32x4_t		down = vdupq_n_s32 (-10 * k);
	uint32x4_t			c, f;

	c = vandq_u32 (vshlq_u32 (pal, down), channel);
	c = vshrq_n_u32 (vmulq_u32 (c, l), 15);
	f = vandq_u32 (vshlq_u32 (fl, down), channel);
	c = vminq_u32 (vmaxq_u32 (c, f), channel);
	return vorrq_u32 (out, vshlq_u32 (c, vdupq_n_s32 (10 * k)));
}

// two lookups a texel are most of it: with the light in lanes and the
// palette's and floor's entries loaded into them, a lane or a pair at a time,
// it ran no faster than the scalar kernel built with the rest
void Simd_M3_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	Simd_Scalar_LitRowRGB (dest, src, palette, floor, light, step, count);
}

// channel k of 4 colors lit as Simd_M3_LitChannel lights them, the floor the
// glow's channel times glowscale
static inline uint32x4_t Simd_M3_GlowChannel (uint32x4_t out, uint32x4_t pix, uint32x4_t glow, uint32x4_t gs,
	uint32x4_t l, int k)
{
	const uint32x4_t	channel = vdupq_n_u32 (1023);
	const int32x4_t		down = vdupq_n_s32 (-10 * k);
	uint32x4_t			c, f;

	c = vshrq_n_u32 (vmulq_u32 (vandq_u32 (vshlq_u32 (pix, down), channel), l), 15);
	f = vshrq_n_u32 (vmulq_u32 (vandq_u32 (vshlq_u32 (glow, down), channel), gs), 15);
	c = vminq_u32 (vmaxq_u32 (c, f), channel);
	return vorrq_u32 (out, vshlq_u32 (c, vdupq_n_s32 (10 * k)));
}

void Simd_M3_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
	const int light[3], const int step[3], int count)
{
	const uint32x4_t	gs = vdupq_n_u32 ((uint32_t)glowscale);
	int32x4_t			l[3], st[3], rev;
	uint32x4_t			pix, gl, out;
	int					j, k;

	if (count < 4)
	{
		Simd_Scalar_LitRowRGB30 (dest, src, glow, glowscale, light, step, count);
		return;
	}
	// the light in registers: the stores to dest could be to it, as far as the
	// compiler knows, and it would be read again every four texels
	for (k = 0 ; k < 3 ; k++)
	{
		l[k] = vdupq_n_s32 (light[k]);
		st[k] = vdupq_n_s32 (step[k]);
	}
	rev = vsubq_s32 (vdupq_n_s32 (count - 1), Simd_M3_Iota ());
	for (j = 0 ; j + 4 <= count ; j += 4, rev = vsubq_s32 (rev, vdupq_n_s32 (4)))
	{
		pix = vld1q_u32 (src + j);
		gl = glow ? vld1q_u32 (glow + j) : vdupq_n_u32 (0);
		out = vdupq_n_u32 (0);
		for (k = 0 ; k < 3 ; k++)
			out = Simd_M3_GlowChannel (out, pix, gl, gs,
				vreinterpretq_u32_s32 (vmaxq_s32 (vmlaq_s32 (l[k], rev, st[k]), vdupq_n_s32 (0))), k);
		vst1q_u32 (dest + j, out);
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

// four words in lanes, put together a pair at a time
SIMD_INLINE uint32x4_t Simd_M3_Pairs (uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	return vreinterpretq_u32_u64 (vcombine_u64 (vcreate_u64 (a | (uint64_t)b << 32), vcreate_u64 (c | (uint64_t)d << 32)));
}

// four pixels at a time, the skin's, remap's, palette's and floor's entries
// loaded into the lanes and the pixels in front blended into the row and the z
// buffer whole; the last few (a span of fewer than four is all of them) the
// scalar kernel's, which steps on from where the four left off as they did
void Simd_M3_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	const int32x4_t		lane = Simd_M3_Iota ();
	const int32x4_t		zistep = vmulq_s32 (lane, vdupq_n_s32 (map->zistep));
	const int32x4_t		lightstep = vmulq_s32 (lane, vdupq_n_s32 (map->lightstep));
	const int32x4_t		sstep = vmulq_s32 (lane, vdupq_n_s32 (map->sfracstep));
	const int32x4_t		tstep = vmulq_s32 (lane, vdupq_n_s32 (map->tfracstep));
	const int32x4_t		whole = vmulq_s32 (lane, vdupq_n_s32 (map->stepwhole));
	const int32x4_t		skinwidth = vdupq_n_s32 (map->skinwidth);
	const int32x4_t		full = vdupq_n_s32 (255 << 6);
	const uint32x4_t	hole = vdupq_n_u32 (255);
	float32x4_t			z, zb;
	uint32x4_t			visible, texel, color, pal, fl, level;
	int32x4_t			s, t, l;
	int					i, k, snext, tnext;

	for (i = 0 ; i + 4 <= count ; i += 4)
	{
		z = vmulq_f32 (vcvtq_f32_s32 (vaddq_s32 (vdupq_n_s32 (zi), zistep)), vdupq_n_f32 (1.0f / 2147483648.0f));
		zb = vld1q_f32 (zbuf + i);
		visible = vcgeq_f32 (z, zb);
		if (vmaxvq_u32 (visible))
		{
			// each pixel's texel: the whole steps and the carries of both fractions
			// (whole steps back are behind tex); the four lookups each pixel
			// takes, the skin's, remap's and then palette's and floor's, are
			// four chains of their own, put together in lanes after
			s = vaddq_s32 (vdupq_n_s32 (sfrac), sstep);
			t = vaddq_s32 (vdupq_n_s32 (tfrac), tstep);
			{
				int32x4_t	at = vaddq_s32 (whole, vmlaq_s32 (vshrq_n_s32 (s, 16), vshrq_n_s32 (t, 16), skinwidth));
				uint32_t	t0 = tex[vgetq_lane_s32 (at, 0)], t1 = tex[vgetq_lane_s32 (at, 1)];
				uint32_t	t2 = tex[vgetq_lane_s32 (at, 2)], t3 = tex[vgetq_lane_s32 (at, 3)];
				uint32_t	r0 = map->remap[t0], r1 = map->remap[t1], r2 = map->remap[t2], r3 = map->remap[t3];

				texel = Simd_M3_Pairs (t0, t1, t2, t3);
				if (map->colormap)
				{
					// each pixel's light, as the lanes have it: four steps a group
					const uint32_t	*cm = map->colormap;
					unsigned		ul = (unsigned)light, us = (unsigned)map->lightstep;

					color = Simd_M3_Pairs (cm[r0 + (ul & 0xFF00)], cm[r1 + ((ul + us) & 0xFF00)],
						cm[r2 + ((ul + 2 * us) & 0xFF00)], cm[r3 + ((ul + 3 * us) & 0xFF00)]);
				}
				else
				{
					pal = Simd_M3_Pairs (map->palette[r0], map->palette[r1], map->palette[r2], map->palette[r3]);
					fl = Simd_M3_Pairs (map->floor[r0], map->floor[r1], map->floor[r2], map->floor[r3]);
				}
			}
			if (map->holey)
				visible = vbicq_u32 (visible, vceqq_u32 (texel, hole));
			if (!map->colormap)
			{
				l = vaddq_s32 (vdupq_n_s32 (light), lightstep);
				level = vbicq_u32 (vreinterpretq_u32_s32 (vsubq_s32 (full, l)), vcgeq_s32 (l, full));
				color = vdupq_n_u32 (0);
				for (k = 0 ; k < 3 ; k++)
					color = Simd_M3_LitChannel (color, pal, fl,
						vshrq_n_u32 (vmulq_u32 (level, vdupq_n_u32 (map->tint[k])), 6), k);
			}
			vst1q_u32 (dest + i, vbslq_u32 (visible, color, vld1q_u32 (dest + i)));
			vst1q_f32 (zbuf + i, vbslq_f32 (visible, z, zb));
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
SIMD_INLINE uint32x4_t Simd_M3_LoadFew (const uint32_t *p, int n)
{
	uint32x4_t	v = vld1q_lane_u32 (p, vdupq_n_u32 (0), 0);

	if (n > 1)
		v = vld1q_lane_u32 (p + 1, v, 1);
	if (n > 2)
		v = vld1q_lane_u32 (p + 2, v, 2);
	return v;
}

SIMD_INLINE float32x4_t Simd_M3_LoadFewF (const float *p, int n)
{
	float32x4_t	v = vld1q_lane_f32 (p, vdupq_n_f32 (0), 0);

	if (n > 1)
		v = vld1q_lane_f32 (p + 1, v, 1);
	if (n > 2)
		v = vld1q_lane_f32 (p + 2, v, 2);
	return v;
}

// the first n (1 to 3) lanes stored
SIMD_INLINE void Simd_M3_StoreFew (uint32_t *p, uint32x4_t v, int n)
{
	vst1q_lane_u32 (p, v, 0);
	if (n > 1)
		vst1q_lane_u32 (p + 1, v, 1);
	if (n > 2)
		vst1q_lane_u32 (p + 2, v, 2);
}

/*
===============================================================================

BLENDING AND 2D

===============================================================================
*/

// one 10 bit channel of src over dest at bit shift, in linear light as
// simd_scalar.c blends it
static inline uint32x4_t Simd_M3_BlendChannel (uint32x4_t s, uint32x4_t d, float32x4_t a, float32x4_t ia,
	int shift)
{
	const uint32x4_t	mask = vdupq_n_u32 (1023);
	const int32x4_t		down = vdupq_n_s32 (-shift);
	float32x4_t			fs = vcvtq_f32_u32 (vandq_u32 (vshlq_u32 (s, down), mask));
	float32x4_t			fd = vcvtq_f32_u32 (vandq_u32 (vshlq_u32 (d, down), mask));
	float32x4_t			x;
	uint32x4_t			c;

	fs = vmulq_f32 (fs, fs);
	fs = vmulq_f32 (fs, fs);
	fd = vmulq_f32 (fd, fd);
	fd = vmulq_f32 (fd, fd);
	x = vmulq_f32 (vaddq_f32 (vmulq_f32 (fs, a), vmulq_f32 (fd, ia)), vdupq_n_f32 (1.0f / 256.0f));
	c = vcvtq_u32_f32 (vaddq_f32 (vsqrtq_f32 (vsqrtq_f32 (x)), vdupq_n_f32 (0.5f)));
	return vshlq_u32 (vminq_u32 (c, mask), vdupq_n_s32 (shift));
}

// four pixels; lanes whose mask is clear keep dest's
static inline uint32x4_t Simd_M3_Blend4 (uint32x4_t s, uint32x4_t d, uint32x4_t m, float32x4_t a,
	float32x4_t ia)
{
	uint32x4_t	out = vorrq_u32 (Simd_M3_BlendChannel (s, d, a, ia, 0),
		vorrq_u32 (Simd_M3_BlendChannel (s, d, a, ia, 10), Simd_M3_BlendChannel (s, d, a, ia, 20)));

	return vbslq_u32 (m, out, d);
}

void Simd_M3_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
	int alpha, int count)
{
	const float32x4_t	vzi = vdupq_n_f32 (zi), vstep = vdupq_n_f32 (step);
	const float32x4_t	a = vdupq_n_f32 ((float)alpha), ia = vdupq_n_f32 ((float)(256 - alpha));
	const uint32x4_t	cutout = vdupq_n_u32 (0x80000000u);
	int32x4_t			idx = Simd_M3_Iota ();
	uint32x4_t			s, m;
	int					i, n;

	for (i = 0 ; i < count ; i += 4, idx = vaddq_s32 (idx, vdupq_n_s32 (4)))
	{
		// the last few a lane at a time; the lanes past them are stored nowhere
		n = count - i < 4 ? count - i : 4;
		s = n == 4 ? vld1q_u32 (src + i) : Simd_M3_LoadFew (src + i, n);
		m = vceqq_u32 (vandq_u32 (s, cutout), vdupq_n_u32 (0));
		if (zbuf)
			m = vandq_u32 (m, vcleq_f32 (n == 4 ? vld1q_f32 (zbuf + i) : Simd_M3_LoadFewF (zbuf + i, n),
				vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep))));
		if (!vmaxvq_u32 (m))
			continue;
		if (n == 4)
			vst1q_u32 (dest + i, Simd_M3_Blend4 (s, vld1q_u32 (dest + i), m, a, ia));
		else
			Simd_M3_StoreFew (dest + i, Simd_M3_Blend4 (s, Simd_M3_LoadFew (dest + i, n), m, a, ia), n);
	}
}

// one 10 bit channel of dest at bit shift: its light times m, plus s, as
// simd_scalar.c adds it
static inline uint32x4_t Simd_M3_PartChannel (uint32x4_t d, float32x4_t s, float32x4_t m, int shift)
{
	const uint32x4_t	mask = vdupq_n_u32 (1023);
	float32x4_t			fd = vcvtq_f32_u32 (vandq_u32 (vshlq_u32 (d, vdupq_n_s32 (-shift)), mask));
	float32x4_t			x;
	uint32x4_t			c;

	fd = vmulq_f32 (fd, fd);
	fd = vmulq_f32 (fd, fd);
	x = vmaxq_f32 (vaddq_f32 (s, vmulq_f32 (fd, m)), vdupq_n_f32 (0));
	c = vcvtq_u32_f32 (vaddq_f32 (vsqrtq_f32 (vsqrtq_f32 (x)), vdupq_n_f32 (0.5f)));
	return vshlq_u32 (vminq_u32 (c, mask), vdupq_n_s32 (shift));
}

void Simd_M3_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
	const float *const src[3], const float *const mul[3], int count)
{
	const float32x4_t	vzi = vdupq_n_f32 (zi), vstep = vdupq_n_f32 (step);
	int32x4_t			idx = Simd_M3_Iota ();
	float32x4_t			sv[3], mv[3];
	uint32x4_t			d, m, out;
	int					i, c, n;

	for (i = 0 ; i < count ; i += 4, idx = vaddq_s32 (idx, vdupq_n_s32 (4)))
	{
		// the last few a lane at a time; the lanes past them are stored nowhere
		n = count - i < 4 ? count - i : 4;
		m = vdupq_n_u32 (0xFFFFFFFFu);
		if (zbuf)
			m = vcleq_f32 (n == 4 ? vld1q_f32 (zbuf + i) : Simd_M3_LoadFewF (zbuf + i, n),
				vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep)));
		if (!vmaxvq_u32 (m))
			continue;
		for (c = 0 ; c < 3 ; c++)
		{
			sv[c] = n == 4 ? vld1q_f32 (src[c] + i) : Simd_M3_LoadFewF (src[c] + i, n);
			mv[c] = n == 4 ? vld1q_f32 (mul[c] + i) : Simd_M3_LoadFewF (mul[c] + i, n);
		}
		d = n == 4 ? vld1q_u32 (dest + i) : Simd_M3_LoadFew (dest + i, n);
		out = vorrq_u32 (Simd_M3_PartChannel (d, sv[0], mv[0], 0),
			vorrq_u32 (Simd_M3_PartChannel (d, sv[1], mv[1], 10), Simd_M3_PartChannel (d, sv[2], mv[2], 20)));
		out = vbslq_u32 (m, out, d);
		if (n == 4)
			vst1q_u32 (dest + i, out);
		else
			Simd_M3_StoreFew (dest + i, out, n);
	}
}

// one 10 bit channel at bit shift fogged, as simd_scalar.c fogs it
static inline uint32x4_t Simd_M3_FogChannel (uint32x4_t p, float32x4_t fog, float32x4_t a, float32x4_t ia,
	int shift)
{
	const uint32x4_t	mask = vdupq_n_u32 (1023);
	float32x4_t			fs = vcvtq_f32_u32 (vandq_u32 (vshlq_u32 (p, vdupq_n_s32 (-shift)), mask));
	float32x4_t			x;
	uint32x4_t			c;

	fs = vmulq_f32 (fs, fs);
	fs = vmulq_f32 (fs, fs);
	x = vmulq_f32 (vaddq_f32 (vmulq_f32 (fs, a), vmulq_f32 (fog, ia)), vdupq_n_f32 (1.0f / 256.0f));
	c = vcvtq_u32_f32 (vaddq_f32 (vsqrtq_f32 (vsqrtq_f32 (x)), vdupq_n_f32 (0.5f)));
	return vshlq_u32 (vminq_u32 (c, mask), vdupq_n_s32 (shift));
}

// four pixels fogged; the lanes whose top bit is set are left as they are
static inline uint32x4_t Simd_M3_Fog4 (uint32x4_t p, float32x4_t z, const simd_fog_t *fog)
{
	const int32x4_t		base = vdupq_n_s32 (fog->base), last = vdupq_n_s32 (fog->size - 1);
	const float32x4_t	full = vdupq_n_f32 (256.0f);
	const uint32_t		*bits = (const uint32_t *)fog->table;	// strict aliasing is off
	int32x4_t			e;
	float32x4_t			a, ia;
	uint32x4_t			out;

	// the table's entries by the bits of 1/z, negative ones the first
	e = vsubq_s32 (vshrq_n_s32 (vreinterpretq_s32_f32 (z), SIMD_FOG_SHIFT), base);
	e = vminq_s32 (vmaxq_s32 (e, vdupq_n_s32 (0)), last);
	a = vreinterpretq_f32_u32 (Simd_M3_Pairs (bits[vgetq_lane_s32 (e, 0)], bits[vgetq_lane_s32 (e, 1)],
		bits[vgetq_lane_s32 (e, 2)], bits[vgetq_lane_s32 (e, 3)]));
	a = vbslq_f32 (vtstq_u32 (p, vdupq_n_u32 (0x40000000u)), vdupq_n_f32 (fog->sky), a);
	ia = vsubq_f32 (full, a);

	out = vorrq_u32 (Simd_M3_FogChannel (p, vdupq_n_f32 (fog->color[0]), a, ia, 0),
		vorrq_u32 (Simd_M3_FogChannel (p, vdupq_n_f32 (fog->color[1]), a, ia, 10),
			Simd_M3_FogChannel (p, vdupq_n_f32 (fog->color[2]), a, ia, 20)));
	return vbslq_u32 (vtstq_u32 (p, vdupq_n_u32 (0x80000000u)), p, out);
}

void Simd_M3_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
	const simd_fog_t *fog)
{
	const float32x4_t	vzi = vdupq_n_f32 (zi), vstep = vdupq_n_f32 (step);
	int32x4_t			idx = Simd_M3_Iota ();
	float32x4_t			z;
	int					i, n;

	for (i = 0 ; i + 4 <= count ; i += 4, idx = vaddq_s32 (idx, vdupq_n_s32 (4)))
	{
		z = zbuf ? vld1q_f32 (zbuf + i) : vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep));
		vst1q_u32 (dest + i, Simd_M3_Fog4 (vld1q_u32 (dest + i), z, fog));
	}
	// the last few a lane at a time; the lanes past them are stored nowhere
	if (i < count)
	{
		n = count - i;
		z = zbuf ? Simd_M3_LoadFewF (zbuf + i, n) : vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep));
		Simd_M3_StoreFew (dest + i, Simd_M3_Fog4 (Simd_M3_LoadFew (dest + i, n), z, fog), n);
	}
}

// the bytes of a vector of four colors that make each of its vectors with
// every color repeated scale times, for vqtbl1q_u8
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

SIMD_INLINE uint32x4_t Simd_M3_Repeat (uint32x4_t v, uint8x16_t tbl)
{
	return vreinterpretq_u32_u8 (vqtbl1q_u8 (vreinterpretq_u8_u32 (v), tbl));
}

// four texels at a time, with scale a constant where it is inlined: their
// colors repeated in place and stored whole, or blended with what is there
// around transparent ones
SIMD_INLINE void Simd_M3_Expand8Scaled (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	const int scale, int transparent)
{
	uint8x16_t	tbl[4];
	uint32x4_t	colors, skip, o;
	uint32_t	*d;
	int			i, q, k;

	for (q = 0 ; q < scale ; q++)
		tbl[q] = vld1q_u8 (expand_tbl[scale][q]);
	for (i = 0 ; i + 4 <= count ; i += 4)
	{
		d = dest + (size_t)i * scale;
		colors = Simd_M3_LookupBytes (palette, src + i);
		if (transparent < 0)
		{
			for (q = 0 ; q < scale ; q++)
				vst1q_u32 (d + 4 * q, scale == 1 ? colors : Simd_M3_Repeat (colors, tbl[q]));
			continue;
		}
		skip = vceqq_u32 (Simd_M3_Widen (src + i), vdupq_n_u32 ((uint32_t)transparent));
		if (vminvq_u32 (skip))
			continue;		// all four skipped
		if (!vmaxvq_u32 (skip))
		{
			for (q = 0 ; q < scale ; q++)
				vst1q_u32 (d + 4 * q, scale == 1 ? colors : Simd_M3_Repeat (colors, tbl[q]));
			continue;
		}
		for (q = 0 ; q < scale ; q++)
		{
			o = vld1q_u32 (d + 4 * q);
			if (scale == 1)
				o = vbslq_u32 (skip, o, colors);
			else
				o = vbslq_u32 (Simd_M3_Repeat (skip, tbl[q]), o, Simd_M3_Repeat (colors, tbl[q]));
			vst1q_u32 (d + 4 * q, o);
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
void Simd_M3_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	switch (scale)
	{
	case 1:
		Simd_M3_Expand8Scaled (dest, src, palette, count, 1, transparent);
		break;
	case 2:
		Simd_M3_Expand8Scaled (dest, src, palette, count, 2, transparent);
		break;
	case 3:
		Simd_M3_Expand8Scaled (dest, src, palette, count, 3, transparent);
		break;
	case 4:
		Simd_M3_Expand8Scaled (dest, src, palette, count, 4, transparent);
		break;
	default:
		Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
		break;
	}
}

// libc's copy is as fast as anything here on Apple silicon
void Simd_M3_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	Simd_Scalar_CopyStream (dest, destpitch, src, srcpitch, rowbytes, rows);
}
