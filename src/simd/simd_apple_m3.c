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

// the compiler vectorizes the scalar kernel as well as this could be
void Simd_M3_ZSpan (float *dest, int count, float zi, float step)
{
	Simd_Scalar_ZSpan (dest, count, zi, step);
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

static void Simd_M3_SpanStart (m3_stepper_t *st, const simd_texmap_t *map, int u, int v, int count, int shift)
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
static void Simd_M3_SpanBatch (m3_stepper_t *st, int first, int n, m3_batch_t *b)
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

void Simd_M3_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	m3_stepper_t	st;
	m3_batch_t		b;
	int				first, n, k, pixels, s, t, sstep, tstep;

	Simd_M3_SpanStart (&st, map, u, v, count, 3);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 4 ? st.subdivisions - first : 4;
		Simd_M3_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++)
		{
			pixels = count - ((first + k) << 3) < 8 ? count - ((first + k) << 3) : 8;
			s = b.sstart[k];
			t = b.tstart[k];
			sstep = b.sstep[k];
			tstep = b.tstep[k];
			for ( ; pixels > 0 ; pixels--, s += sstep, t += tstep)
				*dest++ = src[(s >> 16) + (t >> 16) * srcwidth];
		}
	}
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

/*
===============================================================================

LIGHTING

===============================================================================
*/

// the lookups are all of it: lanes don't help
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

void Simd_M3_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	uint32_t	p[4], f[4], o[4];
	uint32x4_t	pal, fl, out;
	int32x4_t	rev, l;
	int			j, k;

	for (j = 0 ; j < count ; j += 4)
	{
		for (k = 0 ; k < 4 ; k++)
		{
			p[k] = j + k < count ? palette[src[j + k]] : 0;
			f[k] = j + k < count ? floor[src[j + k]] : 0;
		}
		pal = vld1q_u32 (p);
		fl = vld1q_u32 (f);
		rev = vsubq_s32 (vdupq_n_s32 (count - 1 - j), Simd_M3_Iota ());
		out = vdupq_n_u32 (0);
		for (k = 0 ; k < 3 ; k++)
		{
			l = vmaxq_s32 (vmlaq_s32 (vdupq_n_s32 (light[k]), rev, vdupq_n_s32 (step[k])), vdupq_n_s32 (0));
			out = Simd_M3_LitChannel (out, pal, fl, vreinterpretq_u32_s32 (l), k);
		}
		if (j + 4 <= count)
			vst1q_u32 (dest + j, out);
		else
		{
			vst1q_u32 (o, out);
			for (k = 0 ; j + k < count ; k++)
				dest[j + k] = o[k];
		}
	}
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
	uint32_t			p[4], g[4], o[4];
	uint32x4_t			pix, gl, out;
	int32x4_t			rev, l;
	int					j, k;

	for (j = 0 ; j < count ; j += 4)
	{
		if (j + 4 <= count)
		{
			pix = vld1q_u32 (src + j);
			gl = glow ? vld1q_u32 (glow + j) : vdupq_n_u32 (0);
		}
		else
		{
			for (k = 0 ; k < 4 ; k++)
			{
				p[k] = j + k < count ? src[j + k] : 0;
				g[k] = j + k < count && glow ? glow[j + k] : 0;
			}
			pix = vld1q_u32 (p);
			gl = vld1q_u32 (g);
		}
		rev = vsubq_s32 (vdupq_n_s32 (count - 1 - j), Simd_M3_Iota ());
		out = vdupq_n_u32 (0);
		for (k = 0 ; k < 3 ; k++)
		{
			l = vmaxq_s32 (vmlaq_s32 (vdupq_n_s32 (light[k]), rev, vdupq_n_s32 (step[k])), vdupq_n_s32 (0));
			out = Simd_M3_GlowChannel (out, pix, gl, gs, vreinterpretq_u32_s32 (l), k);
		}
		if (j + 4 <= count)
			vst1q_u32 (dest + j, out);
		else
		{
			vst1q_u32 (o, out);
			for (k = 0 ; j + k < count ; k++)
				dest[j + k] = o[k];
		}
	}
}

/*
===============================================================================

MODELS

===============================================================================
*/

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
	float32x4_t			z;
	uint32x4_t			visible, color, pal, fl, level;
	int32x4_t			s, t, l, texel;
	uint32_t			vis[4], out[4], p[4], f[4], idx[4];
	int32_t				at[4], lights[4];
	float				zs[4];
	int					i, k, n, snext, tnext;

	for (i = 0 ; i < count ; i += 4)
	{
		n = count - i < 4 ? count - i : 4;
		z = vmulq_f32 (vcvtq_f32_s32 (vaddq_s32 (vdupq_n_s32 (zi), zistep)), vdupq_n_f32 (1.0f / 2147483648.0f));
		if (n == 4)
			visible = vcgeq_f32 (z, vld1q_f32 (zbuf + i));
		else
		{
			float	zb[4] = {0, 0, 0, 0};

			memcpy (zb, zbuf + i, (size_t)n * sizeof(float));
			visible = vcgeq_f32 (z, vld1q_f32 (zb));
			vst1q_u32 (vis, visible);
			for (k = n ; k < 4 ; k++)
				vis[k] = 0;
			visible = vld1q_u32 (vis);
		}

		if (vmaxvq_u32 (visible))
		{
			// each pixel's texel: the whole steps and the carries of both fractions
			s = vaddq_s32 (vdupq_n_s32 (sfrac), sstep);
			t = vaddq_s32 (vdupq_n_s32 (tfrac), tstep);
			texel = vaddq_s32 (whole, vmlaq_s32 (vshrq_n_s32 (s, 16), vshrq_n_s32 (t, 16), skinwidth));
			l = vaddq_s32 (vdupq_n_s32 (light), lightstep);
			vst1q_u32 (vis, visible);
			vst1q_s32 (at, texel);
			vst1q_s32 (lights, l);
			vst1q_f32 (zs, z);

			if (map->colormap)
			{
				for (k = 0 ; k < n ; k++)
					if (vis[k])
					{
						dest[i + k] = map->colormap[map->remap[tex[at[k]]] + (lights[k] & 0xFF00)];
						zbuf[i + k] = zs[k];
					}
			}
			else
			{
				for (k = 0 ; k < 4 ; k++)
				{
					idx[k] = k < n && vis[k] ? map->remap[tex[at[k]]] : 0;
					p[k] = map->palette[idx[k]];
					f[k] = map->floor[idx[k]];
				}
				pal = vld1q_u32 (p);
				fl = vld1q_u32 (f);
				level = vbicq_u32 (vreinterpretq_u32_s32 (vsubq_s32 (full, l)), vcgeq_s32 (l, full));
				color = vdupq_n_u32 (0);
				for (k = 0 ; k < 3 ; k++)
					color = Simd_M3_LitChannel (color, pal, fl,
						vshrq_n_u32 (vmulq_u32 (level, vdupq_n_u32 (map->tint[k])), 6), k);
				vst1q_u32 (out, color);
				for (k = 0 ; k < n ; k++)
					if (vis[k])
					{
						dest[i + k] = out[k];
						zbuf[i + k] = zs[k];
					}
			}
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
	uint32x4_t			s, d, m;
	uint32_t			sb[4], db[4];
	float				zb[4];
	int					i, n;

	for (i = 0 ; i < count ; i += 4, idx = vaddq_s32 (idx, vdupq_n_s32 (4)))
	{
		n = count - i < 4 ? count - i : 4;
		if (n == 4)
		{
			s = vld1q_u32 (src + i);
			m = vceqq_u32 (vandq_u32 (s, cutout), vdupq_n_u32 (0));
			if (zbuf)
				m = vandq_u32 (m, vcleq_f32 (vld1q_f32 (zbuf + i),
					vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep))));
			if (!vmaxvq_u32 (m))
				continue;
			d = vld1q_u32 (dest + i);
			vst1q_u32 (dest + i, Simd_M3_Blend4 (s, d, m, a, ia));
			continue;
		}

		// the last few, through buffers; the lanes past them blend nothing
		memset (sb, 0xFF, sizeof(sb));
		memset (db, 0, sizeof(db));
		memset (zb, 0, sizeof(zb));
		memcpy (sb, src + i, (size_t)n * 4);
		memcpy (db, dest + i, (size_t)n * 4);
		if (zbuf)
			memcpy (zb, zbuf + i, (size_t)n * 4);
		s = vld1q_u32 (sb);
		m = vceqq_u32 (vandq_u32 (s, cutout), vdupq_n_u32 (0));
		if (zbuf)
			m = vandq_u32 (m, vcleq_f32 (vld1q_f32 (zb), vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep))));
		d = vld1q_u32 (db);
		vst1q_u32 (db, Simd_M3_Blend4 (s, d, m, a, ia));
		memcpy (dest + i, db, (size_t)n * 4);
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
	int32x4_t			e;
	float32x4_t			a, ia;
	float				ab[4];
	uint32x4_t			out;

	// the table's entries by the bits of 1/z, negative ones the first
	e = vsubq_s32 (vshrq_n_s32 (vreinterpretq_s32_f32 (z), SIMD_FOG_SHIFT), base);
	e = vminq_s32 (vmaxq_s32 (e, vdupq_n_s32 (0)), last);
	ab[0] = fog->table[vgetq_lane_s32 (e, 0)];
	ab[1] = fog->table[vgetq_lane_s32 (e, 1)];
	ab[2] = fog->table[vgetq_lane_s32 (e, 2)];
	ab[3] = fog->table[vgetq_lane_s32 (e, 3)];
	a = vbslq_f32 (vtstq_u32 (p, vdupq_n_u32 (0x40000000u)), vdupq_n_f32 (fog->sky), vld1q_f32 (ab));
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
	uint32_t			pb[4];
	float				zb[4];
	int					i, n;

	for (i = 0 ; i < count ; i += 4, idx = vaddq_s32 (idx, vdupq_n_s32 (4)))
	{
		n = count - i < 4 ? count - i : 4;
		if (n == 4)
		{
			z = zbuf ? vld1q_f32 (zbuf + i) : vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep));
			vst1q_u32 (dest + i, Simd_M3_Fog4 (vld1q_u32 (dest + i), z, fog));
			continue;
		}

		// the last few through buffers; the lanes past them are left as they are
		memset (pb, 0xFF, sizeof(pb));
		memcpy (pb, dest + i, (size_t)n * 4);
		if (zbuf)
		{
			memset (zb, 0, sizeof(zb));
			memcpy (zb, zbuf + i, (size_t)n * 4);
			z = vld1q_f32 (zb);
		}
		else
			z = vaddq_f32 (vzi, vmulq_f32 (vcvtq_f32_s32 (idx), vstep));
		vst1q_u32 (pb, Simd_M3_Fog4 (vld1q_u32 (pb), z, fog));
		memcpy (dest + i, pb, (size_t)n * 4);
	}
}

// the lookups and stores are all of it, and the compiler widens the stores
void Simd_M3_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
}

// libc's copy is as fast as anything here on Apple silicon
void Simd_M3_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	Simd_Scalar_CopyStream (dest, destpitch, src, srcpitch, rowbytes, rows);
}
