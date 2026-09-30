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
// simd_x86_64_v3.c -- the kernels for x86-64-v3: AVX2, eight 32 bit lanes. Same
// results as simd_scalar.c, bit for bit: multiplies and adds apart (the FMA the
// level has is never used), square roots and divisions correctly rounded, float
// to int by the same truncation.
//
// AVX2 has gathers, but on the Skylake cores this was timed on (with the
// microcode that mitigates Downfall, which slows gathers down) eight loads a
// lane at a time are faster: what is looked up in a table is, and the
// arithmetic around it is in lanes. Where that doesn't beat the scalar kernel
// as the compiler builds it, the scalar kernel is used. AVX2 has no mask registers either: the
// lanes a kernel works on are a vector of all ones or zeros, which masked loads
// and stores take (and don't touch memory for the rest).

#include "simd_backends.h"

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

static inline __m256i Simd_V3_Iota (void)
{
	return _mm256_setr_epi32 (0, 1, 2, 3, 4, 5, 6, 7);
}

// the first n of 8 lanes
static inline __m256i Simd_V3_Lanes (int n)
{
	return _mm256_cmpgt_epi32 (_mm256_set1_epi32 (n), Simd_V3_Iota ());
}

static inline void Simd_V3_Store (uint32_t *dest, __m256i v, int n)
{
	if (n >= 8)
		_mm256_storeu_si256 ((__m256i *)dest, v);
	else
		_mm256_maskstore_epi32 ((int *)dest, Simd_V3_Lanes (n), v);
}

// the compiler vectorizes the scalar kernel as well as this could be
void Simd_V3_ZSpan (float *dest, int count, float zi, float step)
{
	Simd_Scalar_ZSpan (dest, count, zi, step);
}

/*
===============================================================================

PERSPECTIVE SPANS

===============================================================================
*/

// the perspective stepping of simd_scalar.c, eight subdivisions at a time: the
// running sums of s/z, t/z and 1/z stay serial, the division and the rest are
// in lanes
typedef struct
{
	const simd_texmap_t	*map;
	float		sdivz, tdivz, zi;
	int			s, t;				// where the next subdivision starts
	int			count, shift;
	int			subdivisions;
} v3_stepper_t;

// s and t at the start of each subdivision of a batch, and their steps
typedef struct
{
	int32_t		sstart[8], sstep[8], tstart[8], tstep[8];
} v3_batch_t;

static int Simd_V3_ClampScalar (int x, int lo, int hi)
{
	if (x > hi)
		return hi;
	if (x < lo)
		return lo;
	return x;
}

// hi if above it, else lo if below it
static inline __m256i Simd_V3_Clamp (__m256i x, int lo, int hi)
{
	__m256i	vhi = _mm256_set1_epi32 (hi);

	return _mm256_blendv_epi8 (_mm256_max_epi32 (x, _mm256_set1_epi32 (lo)), vhi, _mm256_cmpgt_epi32 (x, vhi));
}

static void Simd_V3_SpanStart (v3_stepper_t *st, const simd_texmap_t *map, int u, int v, int count, int shift)
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
	st->s = Simd_V3_ClampScalar ((int)(st->sdivz * z) + map->sadjust, 0, map->sextent);
	st->t = Simd_V3_ClampScalar ((int)(st->tdivz * z) + map->tadjust, 0, map->textent);
}

// subdivisions first .. first+n-1 (n at most 8), in lanes 0 .. n-1
static void Simd_V3_SpanBatch (v3_stepper_t *st, int first, int n, v3_batch_t *b)
{
	const simd_texmap_t	*map = st->map;
	float		sd[8] = {0}, td[8] = {0}, zd[8] = {1, 1, 1, 1, 1, 1, 1, 1}, m1;
	int32_t		sn[8], tn[8];
	int			k, len, step = 1 << st->shift, last = st->subdivisions - 1;
	__m128i		shift = _mm_cvtsi32_si128 (st->shift);
	__m256		z;
	__m256i		vs, vt;

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
	z = _mm256_div_ps (_mm256_set1_ps ((float)0x10000), _mm256_loadu_ps (zd));
	vs = _mm256_add_epi32 (_mm256_cvttps_epi32 (_mm256_mul_ps (_mm256_loadu_ps (sd), z)),
		_mm256_set1_epi32 (map->sadjust));
	vt = _mm256_add_epi32 (_mm256_cvttps_epi32 (_mm256_mul_ps (_mm256_loadu_ps (td), z)),
		_mm256_set1_epi32 (map->tadjust));
	_mm256_storeu_si256 ((__m256i *)sn, Simd_V3_Clamp (vs, step, map->sextent));
	_mm256_storeu_si256 ((__m256i *)tn, Simd_V3_Clamp (vt, step, map->textent));

	// each subdivision starts where the one before ends
	b->sstart[0] = st->s;
	b->tstart[0] = st->t;
	for (k = 1 ; k < 8 ; k++)
	{
		b->sstart[k] = sn[k - 1];
		b->tstart[k] = tn[k - 1];
	}
	_mm256_storeu_si256 ((__m256i *)b->sstep, _mm256_sra_epi32 (_mm256_sub_epi32 (_mm256_loadu_si256 (
		(const __m256i *)sn), _mm256_loadu_si256 ((const __m256i *)b->sstart)), shift));
	_mm256_storeu_si256 ((__m256i *)b->tstep, _mm256_sra_epi32 (_mm256_sub_epi32 (_mm256_loadu_si256 (
		(const __m256i *)tn), _mm256_loadu_si256 ((const __m256i *)b->tstart)), shift));

	// the last subdivision's steps by division
	if (first + n - 1 == last)
	{
		k = n - 1;
		len = st->count - (last << st->shift);
		if (len > 1)
		{
			b->sstep[k] = (sn[k] - b->sstart[k]) / (len - 1);
			b->tstep[k] = (tn[k] - b->tstart[k]) / (len - 1);
		}
	}
	st->s = sn[n - 1];
	st->t = tn[n - 1];
}

// the texels' offsets in lanes, a subdivision of 8 pixels at a time; the
// texels looked up one by one
void Simd_V3_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	const __m256i	lane = Simd_V3_Iota ();
	const __m256i	width = _mm256_set1_epi32 (srcwidth);
	v3_stepper_t	st;
	v3_batch_t		b;
	int32_t			at[8];
	int				first, n, k, j, pixels;
	__m256i			s, t;

	Simd_V3_SpanStart (&st, map, u, v, count, 3);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 8 ? st.subdivisions - first : 8;
		Simd_V3_SpanBatch (&st, first, n, &b);
		for (k = 0 ; k < n ; k++, dest += 8)
		{
			pixels = count - ((first + k) << 3) < 8 ? count - ((first + k) << 3) : 8;
			s = _mm256_add_epi32 (_mm256_set1_epi32 (b.sstart[k]), _mm256_mullo_epi32 (lane,
				_mm256_set1_epi32 (b.sstep[k])));
			t = _mm256_add_epi32 (_mm256_set1_epi32 (b.tstart[k]), _mm256_mullo_epi32 (lane,
				_mm256_set1_epi32 (b.tstep[k])));
			_mm256_storeu_si256 ((__m256i *)at, _mm256_add_epi32 (_mm256_srai_epi32 (s, 16),
				_mm256_mullo_epi32 (_mm256_srai_epi32 (t, 16), width)));
			for (j = 0 ; j < pixels ; j++)
				dest[j] = src[at[j]];
		}
	}
}

// the stepping above, eight subdivisions at a time; the pixels as the scalar
// kernel steps them (four lookups a pixel, in lanes they were half as fast)
void Simd_V3_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	v3_stepper_t	st;
	v3_batch_t		b;
	int				first, n, k, pixels, s, t, sstep, tstep, sturb, tturb;

	Simd_V3_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n)
	{
		n = st.subdivisions - first < 8 ? st.subdivisions - first : 8;
		Simd_V3_SpanBatch (&st, first, n, &b);
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

/*
===============================================================================

LIGHTING

===============================================================================
*/

// the lookups are all of it: lanes don't help
void Simd_V3_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	Simd_Scalar_LitRowColormap (dest, src, colormap, light, step, count);
}

// channel k of 8 colors lit (light 32768 being 1.0), at least the floor's, at
// most 1023, or'd into out
static inline __m256i Simd_V3_LitChannel (__m256i out, __m256i pal, __m256i fl, __m256i l, int k)
{
	const __m256i	channel = _mm256_set1_epi32 (1023);
	__m128i			shift = _mm_cvtsi32_si128 (10 * k);
	__m256i			c, f;

	c = _mm256_and_si256 (_mm256_srl_epi32 (pal, shift), channel);
	c = _mm256_srli_epi32 (_mm256_mullo_epi32 (c, l), 15);
	f = _mm256_and_si256 (_mm256_srl_epi32 (fl, shift), channel);
	c = _mm256_min_epu32 (_mm256_max_epu32 (c, f), channel);
	return _mm256_or_si256 (out, _mm256_sll_epi32 (c, shift));
}

void Simd_V3_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	uint32_t	p[16], f[16];
	__m256i		rev, out, l;
	int			h, j, k;

	for (j = 0 ; j < count ; j++)
	{
		p[j] = palette[src[j]];
		f[j] = floor[src[j]];
	}
	for (h = 0 ; h < count ; h += 8)
	{
		rev = _mm256_sub_epi32 (_mm256_set1_epi32 (count - 1 - h), Simd_V3_Iota ());
		out = _mm256_setzero_si256 ();
		for (k = 0 ; k < 3 ; k++)
		{
			l = _mm256_add_epi32 (_mm256_set1_epi32 (light[k]), _mm256_mullo_epi32 (rev, _mm256_set1_epi32 (step[k])));
			out = Simd_V3_LitChannel (out, _mm256_loadu_si256 ((const __m256i *)(p + h)),
				_mm256_loadu_si256 ((const __m256i *)(f + h)), _mm256_max_epi32 (l, _mm256_setzero_si256 ()), k);
		}
		Simd_V3_Store (dest + h, out, count - h);
	}
}

/*
===============================================================================

MODELS

===============================================================================
*/

// z, the texel steps and the light in lanes; the texel, its remap and the
// colormap or palette a pixel at a time
void Simd_V3_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	const __m256i	lane = Simd_V3_Iota ();
	const __m256i	full = _mm256_set1_epi32 (255 << 6);
	const __m256i	zistep = _mm256_mullo_epi32 (lane, _mm256_set1_epi32 (map->zistep));
	const __m256i	lightstep = _mm256_mullo_epi32 (lane, _mm256_set1_epi32 (map->lightstep));
	const __m256i	sstep = _mm256_mullo_epi32 (lane, _mm256_set1_epi32 (map->sfracstep));
	const __m256i	tstep = _mm256_mullo_epi32 (lane, _mm256_set1_epi32 (map->tfracstep));
	const __m256i	whole = _mm256_mullo_epi32 (lane, _mm256_set1_epi32 (map->stepwhole));
	const __m256i	skinwidth = _mm256_set1_epi32 (map->skinwidth);
	__m256			z;
	__m256i			lanes, visible, s, t, l, color, level;
	int32_t			at[8], lights[8];
	uint32_t		p[8], f[8], index;
	float			zs[8];
	int				i, k, n, shown, snext, tnext;

	for (i = 0 ; i < count ; i += 8)
	{
		n = count - i < 8 ? count - i : 8;
		lanes = Simd_V3_Lanes (n);
		z = _mm256_mul_ps (_mm256_cvtepi32_ps (_mm256_add_epi32 (_mm256_set1_epi32 (zi), zistep)),
			_mm256_set1_ps (1.0f / 2147483648.0f));
		visible = _mm256_and_si256 (lanes, _mm256_castps_si256 (_mm256_cmp_ps (z,
			_mm256_maskload_ps (zbuf + i, lanes), _CMP_GE_OQ)));
		shown = _mm256_movemask_ps (_mm256_castsi256_ps (visible));

		if (shown)
		{
			// each pixel's texel: the whole steps and the carries of both fractions
			s = _mm256_add_epi32 (_mm256_set1_epi32 (sfrac), sstep);
			t = _mm256_add_epi32 (_mm256_set1_epi32 (tfrac), tstep);
			_mm256_storeu_si256 ((__m256i *)at, _mm256_add_epi32 (whole, _mm256_add_epi32 (_mm256_srai_epi32 (s, 16),
				_mm256_mullo_epi32 (_mm256_srai_epi32 (t, 16), skinwidth))));
			l = _mm256_add_epi32 (_mm256_set1_epi32 (light), lightstep);

			if (map->colormap)
			{
				_mm256_storeu_si256 ((__m256i *)lights, l);
				_mm256_storeu_ps (zs, z);
				for (k = 0 ; k < n ; k++)
					if (shown & (1 << k))
					{
						dest[i + k] = map->colormap[map->remap[tex[at[k]]] + (lights[k] & 0xFF00)];
						zbuf[i + k] = zs[k];
					}
			}
			else
			{
				for (k = 0 ; k < 8 ; k++)
				{
					index = (shown & (1 << k)) ? map->remap[tex[at[k]]] : 0;
					p[k] = map->palette[index];
					f[k] = map->floor[index];
				}
				// 0 where the light is past full: the level's lanes where it isn't
				level = _mm256_and_si256 (_mm256_sub_epi32 (full, l), _mm256_cmpgt_epi32 (full, l));
				color = _mm256_setzero_si256 ();
				for (k = 0 ; k < 3 ; k++)
					color = Simd_V3_LitChannel (color, _mm256_loadu_si256 ((const __m256i *)p),
						_mm256_loadu_si256 ((const __m256i *)f), _mm256_srli_epi32 (_mm256_mullo_epi32 (level,
						_mm256_set1_epi32 ((int)map->tint[k])), 6), k);
				_mm256_maskstore_epi32 ((int *)(dest + i), visible, color);
				_mm256_maskstore_ps (zbuf + i, visible, z);
			}
		}

		// eight pixels on
		zi = (int)((unsigned)zi + 8u * (unsigned)map->zistep);
		light = (int)((unsigned)light + 8u * (unsigned)map->lightstep);
		snext = sfrac + 8 * map->sfracstep;
		tnext = tfrac + 8 * map->tfracstep;
		tex += 8 * map->stepwhole + (snext >> 16) + (tnext >> 16) * map->skinwidth;
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
static inline __m256i Simd_V3_BlendChannel (__m256i s, __m256i d, __m256 a, __m256 ia, int shift)
{
	const __m256i	mask = _mm256_set1_epi32 (1023);
	__m128i			count = _mm_cvtsi32_si128 (shift);
	__m256			fs = _mm256_cvtepi32_ps (_mm256_and_si256 (_mm256_srl_epi32 (s, count), mask));
	__m256			fd = _mm256_cvtepi32_ps (_mm256_and_si256 (_mm256_srl_epi32 (d, count), mask));
	__m256			x;
	__m256i			c;

	fs = _mm256_mul_ps (fs, fs);
	fs = _mm256_mul_ps (fs, fs);
	fd = _mm256_mul_ps (fd, fd);
	fd = _mm256_mul_ps (fd, fd);
	x = _mm256_mul_ps (_mm256_add_ps (_mm256_mul_ps (fs, a), _mm256_mul_ps (fd, ia)), _mm256_set1_ps (1.0f / 256.0f));
	c = _mm256_cvttps_epi32 (_mm256_add_ps (_mm256_sqrt_ps (_mm256_sqrt_ps (x)), _mm256_set1_ps (0.5f)));
	return _mm256_sll_epi32 (_mm256_min_epi32 (c, mask), count);
}

void Simd_V3_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
	int alpha, int count)
{
	const __m256	lane = _mm256_cvtepi32_ps (Simd_V3_Iota ());
	const __m256	vzi = _mm256_set1_ps (zi);
	const __m256	vstep = _mm256_set1_ps (step);
	const __m256	a = _mm256_set1_ps ((float)alpha);
	const __m256	ia = _mm256_set1_ps ((float)(256 - alpha));
	const __m256i	cutout = _mm256_set1_epi32 ((int)0x80000000u);
	__m256i			s, d, out, m;
	int				i;

	for (i = 0 ; i < count ; i += 8)
	{
		// the lanes of the span, not cut out, and in front of the z buffer
		m = Simd_V3_Lanes (count - i);
		s = _mm256_maskload_epi32 ((const int *)(src + i), m);
		m = _mm256_andnot_si256 (_mm256_cmpeq_epi32 (_mm256_and_si256 (s, cutout), cutout), m);
		if (zbuf)
		{
			__m256	idx = _mm256_add_ps (_mm256_set1_ps ((float)i), lane);	// whole numbers, exact
			__m256	z = _mm256_add_ps (vzi, _mm256_mul_ps (idx, vstep));

			m = _mm256_and_si256 (m, _mm256_castps_si256 (_mm256_cmp_ps (_mm256_maskload_ps (zbuf + i, m), z,
				_CMP_LE_OQ)));
		}
		if (_mm256_testz_si256 (m, m))
			continue;
		d = _mm256_maskload_epi32 ((const int *)(dest + i), m);
		out = _mm256_or_si256 (Simd_V3_BlendChannel (s, d, a, ia, 0),
			_mm256_or_si256 (Simd_V3_BlendChannel (s, d, a, ia, 10), Simd_V3_BlendChannel (s, d, a, ia, 20)));
		_mm256_maskstore_epi32 ((int *)(dest + i), m, out);
	}
}

// the lookups and stores are all of it, and the compiler widens the stores
void Simd_V3_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
}

void Simd_V3_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	byte		*d;
	const byte	*s;
	size_t		head, bytes;
	int			y;

	for (y = 0 ; y < rows ; y++)
	{
		d = (byte *)dest + y * destpitch;
		s = (const byte *)src + y * srcpitch;
		bytes = rowbytes;

		// streaming stores need 32 byte aligned destinations
		head = (32 - ((uintptr_t)d & 31)) & 31;
		if (head > bytes)
			head = bytes;
		memcpy (d, s, head);
		d += head;
		s += head;
		bytes -= head;

		for ( ; bytes >= 32 ; bytes -= 32, d += 32, s += 32)
			_mm256_stream_si256 ((__m256i *)d, _mm256_loadu_si256 ((const __m256i *)s));
		memcpy (d, s, bytes);
	}
	_mm_sfence ();
}

// these are the scalar kernels' until they are vectorized here
void Simd_V3_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
	const simd_fog_t *fog)
{
	Simd_Scalar_FogSpan (dest, zbuf, zi, step, count, fog);
}

void Simd_V3_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
	const int *turb, int u, int v, int count)
{
	Simd_Scalar_TurbSpanRGB30 (dest, map, src, turb, u, v, count);
}

void Simd_V3_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
	const int light[3], const int step[3], int count)
{
	Simd_Scalar_LitRowRGB30 (dest, src, glow, glowscale, light, step, count);
}
