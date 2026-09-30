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
// simd_x86_64_v4.c -- the kernels for x86-64-v4: AVX-512 F/BW/CD/DQ/VL,
// sixteen 32 bit lanes. Same results as simd_scalar.c, bit for bit.

#include "simd_backends.h"

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

// the first n of 16 lanes
static inline __mmask16 Simd_V4_Lanes (int n)
{
	return n >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << n) - 1);
}

static inline __m512i Simd_V4_Iota (void)
{
	return _mm512_setr_epi32 (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
}

// count 8 bit texels, zero extended to 32 bit lanes
static inline __m512i Simd_V4_LoadTexels (const byte *src, __mmask16 lanes)
{
	return _mm512_cvtepu8_epi32 (_mm_maskz_loadu_epi8 (lanes, src));
}

void Simd_V4_ZSpan (float *dest, int count, float zi, float step)
{
	const __m512	lane = _mm512_cvtepi32_ps (Simd_V4_Iota ());
	const __m512	vzi = _mm512_set1_ps (zi);
	const __m512	vstep = _mm512_set1_ps (step);
	int				i;

	for (i = 0 ; i < count ; i += 16)
	{
		__m512	idx = _mm512_add_ps (_mm512_set1_ps ((float)i), lane);	// whole numbers, exact

		_mm512_mask_storeu_ps (dest + i, Simd_V4_Lanes (count - i),
			_mm512_add_ps (vzi, _mm512_mul_ps (idx, vstep)));
	}
}

// the perspective stepping of simd_scalar.c, sixteen subdivisions at a time:
// the running sums of s/z, t/z and 1/z stay serial, the rest is in lanes
typedef struct
{
	const simd_texmap_t	*map;
	float		sdivz, tdivz, zi;
	int			s, t;				// where the next subdivision starts
	int			count, shift;
	int			subdivisions;
} v4_stepper_t;

// s and t at the start of each subdivision of a batch, and their steps
typedef struct
{
	__m512i		sstart, sstep, tstart, tstep;
} v4_batch_t;

// lane k of v
static inline int Simd_V4_Lane (__m512i v, int k)
{
	return _mm_cvtsi128_si32 (_mm512_castsi512_si128 (_mm512_permutexvar_epi32 (_mm512_set1_epi32 (k), v)));
}

// hi if above it, else lo if below it
static inline __m512i Simd_V4_Clamp (__m512i x, int lo, int hi)
{
	__m512i	vhi = _mm512_set1_epi32 (hi);

	return _mm512_mask_blend_epi32 (_mm512_cmpgt_epi32_mask (x, vhi), _mm512_max_epi32 (x, _mm512_set1_epi32 (lo)), vhi);
}

static int Simd_V4_ClampScalar (int x, int lo, int hi)
{
	if (x > hi)
		return hi;
	if (x < lo)
		return lo;
	return x;
}

static void Simd_V4_SpanStart (v4_stepper_t *st, const simd_texmap_t *map, int u, int v,
	int count, int shift)
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
	st->s = Simd_V4_ClampScalar ((int)(st->sdivz * z) + map->sadjust, 0, map->sextent);
	st->t = Simd_V4_ClampScalar ((int)(st->tdivz * z) + map->tadjust, 0, map->textent);
}

// subdivisions first .. first+n-1 (n at most 16), in lanes 0 .. n-1
static v4_batch_t Simd_V4_SpanBatch (v4_stepper_t *st, int first, int n)
{
	const simd_texmap_t	*map = st->map;
	float		m1;
	__m512		sd = _mm512_setzero_ps (), td = _mm512_setzero_ps (), zd = _mm512_set1_ps (1.0f);
	int			k, len, step = 1 << st->shift, last = st->subdivisions - 1;
	__mmask16	lanes = Simd_V4_Lanes (n);
	__m128i		shift = _mm_cvtsi32_si128 (st->shift);
	__m512		z;
	__m512i		sn, tn;
	v4_batch_t	b;

	// where each subdivision ends; the last one ends on the span's last pixel
	for (k = 0 ; k < n ; k++)
	{
		if (first + k < last)
		{
			st->sdivz += map->sdivzstepu * (float)step;
			st->tdivz += map->tdivzstepu * (float)step;
			st->zi += map->zistepu * (float)step;
			sd = _mm512_mask_mov_ps (sd, (__mmask16)(1u << k), _mm512_set1_ps (st->sdivz));
			td = _mm512_mask_mov_ps (td, (__mmask16)(1u << k), _mm512_set1_ps (st->tdivz));
			zd = _mm512_mask_mov_ps (zd, (__mmask16)(1u << k), _mm512_set1_ps (st->zi));
		}
		else
		{
			m1 = (float)(st->count - (last << st->shift) - 1);
			sd = _mm512_mask_mov_ps (sd, (__mmask16)(1u << k), _mm512_set1_ps (st->sdivz + map->sdivzstepu * m1));
			td = _mm512_mask_mov_ps (td, (__mmask16)(1u << k), _mm512_set1_ps (st->tdivz + map->tdivzstepu * m1));
			zd = _mm512_mask_mov_ps (zd, (__mmask16)(1u << k), _mm512_set1_ps (st->zi + map->zistepu * m1));
		}
	}
	z = _mm512_mask_div_ps (_mm512_set1_ps (1.0f), lanes, _mm512_set1_ps ((float)0x10000), zd);
	sn = _mm512_add_epi32 (_mm512_cvttps_epi32 (_mm512_mul_ps (sd, z)), _mm512_set1_epi32 (map->sadjust));
	tn = _mm512_add_epi32 (_mm512_cvttps_epi32 (_mm512_mul_ps (td, z)), _mm512_set1_epi32 (map->tadjust));
	sn = Simd_V4_Clamp (sn, step, map->sextent);
	tn = Simd_V4_Clamp (tn, step, map->textent);

	// each subdivision starts where the one before ends
	b.sstart = _mm512_alignr_epi32 (sn, _mm512_set1_epi32 (st->s), 15);
	b.tstart = _mm512_alignr_epi32 (tn, _mm512_set1_epi32 (st->t), 15);
	b.sstep = _mm512_sra_epi32 (_mm512_sub_epi32 (sn, b.sstart), shift);
	b.tstep = _mm512_sra_epi32 (_mm512_sub_epi32 (tn, b.tstart), shift);

	// the last subdivision's steps by division
	if (first + n - 1 == last)
	{
		k = n - 1;
		len = st->count - (last << st->shift);
		if (len > 1)
		{
			b.sstep = _mm512_mask_set1_epi32 (b.sstep, (__mmask16)(1u << k),
				(Simd_V4_Lane (sn, k) - Simd_V4_Lane (b.sstart, k)) / (len - 1));
			b.tstep = _mm512_mask_set1_epi32 (b.tstep, (__mmask16)(1u << k),
				(Simd_V4_Lane (tn, k) - Simd_V4_Lane (b.tstart, k)) / (len - 1));
		}
	}
	st->s = Simd_V4_Lane (sn, n - 1);
	st->t = Simd_V4_Lane (tn, n - 1);
	return b;
}

void Simd_V4_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	v4_stepper_t	st;
	v4_batch_t		b;
	int				first, n, p, pixels;
	__mmask16		lanes;
	__m512i			lane = Simd_V4_Iota ();
	__m512i			sub = _mm512_srli_epi32 (lane, 3);		// two subdivisions per vector
	__m512i			within = _mm512_and_si512 (lane, _mm512_set1_epi32 (7));
	__m512i			width = _mm512_set1_epi32 (srcwidth);
	__m512i			which, s, t, idx;

	Simd_V4_SpanStart (&st, map, u, v, count, 3);
	for (first = 0 ; first < st.subdivisions ; first += n, dest += pixels)
	{
		n = st.subdivisions - first < 16 ? st.subdivisions - first : 16;
		b = Simd_V4_SpanBatch (&st, first, n);
		pixels = count - (first << 3) < (n << 3) ? count - (first << 3) : n << 3;
		for (p = 0 ; p < pixels ; p += 16)
		{
			which = _mm512_add_epi32 (sub, _mm512_set1_epi32 (p >> 3));
			s = _mm512_add_epi32 (_mm512_permutexvar_epi32 (which, b.sstart),
				_mm512_mullo_epi32 (within, _mm512_permutexvar_epi32 (which, b.sstep)));
			t = _mm512_add_epi32 (_mm512_permutexvar_epi32 (which, b.tstart),
				_mm512_mullo_epi32 (within, _mm512_permutexvar_epi32 (which, b.tstep)));
			idx = _mm512_add_epi32 (_mm512_srai_epi32 (s, 16), _mm512_mullo_epi32 (_mm512_srai_epi32 (t, 16), width));
			lanes = Simd_V4_Lanes (pixels - p);
			_mm512_mask_storeu_epi32 (dest + p, lanes,
				_mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, idx, src, 4));
		}
	}
}

void Simd_V4_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	v4_stepper_t	st;
	v4_batch_t		b;
	int				first, n, p, pixels;
	__mmask16		lanes;
	__m512i			lane = Simd_V4_Iota ();
	__m512i			cycle = _mm512_set1_epi32 (127), size = _mm512_set1_epi32 (63);
	__m512i			wrap = _mm512_set1_epi32 ((128 << 16) - 1);
	__m512i			which, s, t, sturb, tturb, texel;

	Simd_V4_SpanStart (&st, map, u, v, count, 4);
	for (first = 0 ; first < st.subdivisions ; first += n, dest += pixels)
	{
		n = st.subdivisions - first < 16 ? st.subdivisions - first : 16;
		b = Simd_V4_SpanBatch (&st, first, n);
		pixels = count - (first << 4) < (n << 4) ? count - (first << 4) : n << 4;
		// one subdivision per vector, its start wrapped to the wave's cycle
		for (p = 0 ; p < pixels ; p += 16)
		{
			which = _mm512_set1_epi32 (p >> 4);
			s = _mm512_add_epi32 (_mm512_and_si512 (_mm512_permutexvar_epi32 (which, b.sstart), wrap),
				_mm512_mullo_epi32 (lane, _mm512_permutexvar_epi32 (which, b.sstep)));
			t = _mm512_add_epi32 (_mm512_and_si512 (_mm512_permutexvar_epi32 (which, b.tstart), wrap),
				_mm512_mullo_epi32 (lane, _mm512_permutexvar_epi32 (which, b.tstep)));
			sturb = _mm512_i32gather_epi32 (_mm512_and_si512 (_mm512_srai_epi32 (t, 16), cycle), turb, 4);
			tturb = _mm512_i32gather_epi32 (_mm512_and_si512 (_mm512_srai_epi32 (s, 16), cycle), turb, 4);
			sturb = _mm512_and_si512 (_mm512_srai_epi32 (_mm512_add_epi32 (s, sturb), 16), size);
			tturb = _mm512_and_si512 (_mm512_srai_epi32 (_mm512_add_epi32 (t, tturb), 16), size);
			lanes = Simd_V4_Lanes (pixels - p);
			// 32 bit loads of the 8 bit texels, hence the 3 readable bytes after src
			texel = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes,
				_mm512_add_epi32 (_mm512_slli_epi32 (tturb, 6), sturb), src, 1);
			texel = _mm512_and_si512 (texel, _mm512_set1_epi32 (0xFF));
			_mm512_mask_storeu_epi32 (dest + p, lanes,
				_mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, texel, palette, 4));
		}
	}
}

void Simd_V4_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	__mmask16	lanes = Simd_V4_Lanes (count);
	__m512i		rev, l, idx;

	rev = _mm512_sub_epi32 (_mm512_set1_epi32 (count - 1), Simd_V4_Iota ());
	l = _mm512_add_epi32 (_mm512_set1_epi32 (light), _mm512_mullo_epi32 (rev, _mm512_set1_epi32 (step)));
	idx = _mm512_add_epi32 (_mm512_and_si512 (l, _mm512_set1_epi32 (0xFF00)), Simd_V4_LoadTexels (src, lanes));
	_mm512_mask_storeu_epi32 (dest, lanes,
		_mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, idx, colormap, 4));
}

void Simd_V4_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	__mmask16	lanes = Simd_V4_Lanes (count);
	__m512i		tex, pal, fl, rev, out, l, c, f;
	__m512i		channel = _mm512_set1_epi32 (1023);
	int			k;

	tex = Simd_V4_LoadTexels (src, lanes);
	pal = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, tex, palette, 4);
	fl = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, tex, floor, 4);
	rev = _mm512_sub_epi32 (_mm512_set1_epi32 (count - 1), Simd_V4_Iota ());
	out = _mm512_setzero_si512 ();

	for (k = 0 ; k < 3 ; k++)
	{
		__m128i	shift = _mm_cvtsi32_si128 (10 * k);

		l = _mm512_add_epi32 (_mm512_set1_epi32 (light[k]), _mm512_mullo_epi32 (rev, _mm512_set1_epi32 (step[k])));
		l = _mm512_max_epi32 (l, _mm512_setzero_si512 ());
		c = _mm512_and_si512 (_mm512_srl_epi32 (pal, shift), channel);
		c = _mm512_srli_epi32 (_mm512_mullo_epi32 (c, l), 15);
		f = _mm512_and_si512 (_mm512_srl_epi32 (fl, shift), channel);
		c = _mm512_min_epu32 (_mm512_max_epu32 (c, f), channel);
		out = _mm512_or_si512 (out, _mm512_sll_epi32 (c, shift));
	}
	_mm512_mask_storeu_epi32 (dest, lanes, out);
}

// the bytes at base + offset, read as the aligned dwords that hold them, so no
// read reaches past the dword of a byte that is there
static inline __m512i Simd_V4_GatherBytes (const byte *base, __m512i offset, __mmask16 lanes)
{
	const byte	*aligned = (const byte *)((uintptr_t)base & ~(uintptr_t)3);
	__m512i		at = _mm512_add_epi32 (offset, _mm512_set1_epi32 ((int)((uintptr_t)base & 3)));
	__m512i		dw = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, _mm512_srai_epi32 (at, 2), aligned, 4);

	dw = _mm512_srlv_epi32 (dw, _mm512_slli_epi32 (_mm512_and_si512 (at, _mm512_set1_epi32 (3)), 3));
	return _mm512_and_si512 (dw, _mm512_set1_epi32 (0xFF));
}

void Simd_V4_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	const __m512i	lane = Simd_V4_Iota ();
	const __m512i	channel = _mm512_set1_epi32 (1023), full = _mm512_set1_epi32 (255 << 6);
	const __m512i	zistep = _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (map->zistep));
	const __m512i	lightstep = _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (map->lightstep));
	const __m512i	sstep = _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (map->sfracstep));
	const __m512i	tstep = _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (map->tfracstep));
	const __m512i	whole = _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (map->stepwhole));
	const __m512i	skinwidth = _mm512_set1_epi32 (map->skinwidth);
	__mmask16		lanes, visible;
	__m512			z;
	__m512i			s, t, l, index, color, pal, fl, level, c, f;
	int				i, k, snext, tnext;

	for (i = 0 ; i < count ; i += 16)
	{
		lanes = Simd_V4_Lanes (count - i);
		z = _mm512_mul_ps (_mm512_cvtepi32_ps (_mm512_add_epi32 (_mm512_set1_epi32 (zi), zistep)),
			_mm512_set1_ps (1.0f / 2147483648.0f));
		visible = _mm512_mask_cmp_ps_mask (lanes, z, _mm512_maskz_loadu_ps (lanes, zbuf + i), _CMP_GE_OQ);

		if (visible)
		{
			// each pixel's texel: the whole steps and the carries of both fractions
			s = _mm512_add_epi32 (_mm512_set1_epi32 (sfrac), sstep);
			t = _mm512_add_epi32 (_mm512_set1_epi32 (tfrac), tstep);
			index = _mm512_add_epi32 (whole, _mm512_add_epi32 (_mm512_srai_epi32 (s, 16),
				_mm512_mullo_epi32 (_mm512_srai_epi32 (t, 16), skinwidth)));
			index = Simd_V4_GatherBytes (map->remap, Simd_V4_GatherBytes (tex, index, visible), visible);
			l = _mm512_add_epi32 (_mm512_set1_epi32 (light), lightstep);

			if (map->colormap)
				color = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), visible,
					_mm512_add_epi32 (index, _mm512_and_si512 (l, _mm512_set1_epi32 (0xFF00))), map->colormap, 4);
			else
			{
				level = _mm512_maskz_sub_epi32 (_mm512_cmplt_epi32_mask (l, full), full, l);
				pal = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), visible, index, map->palette, 4);
				fl = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), visible, index, map->floor, 4);
				color = _mm512_setzero_si512 ();
				for (k = 0 ; k < 3 ; k++)
				{
					__m128i	shift = _mm_cvtsi32_si128 (10 * k);

					l = _mm512_srli_epi32 (_mm512_mullo_epi32 (level, _mm512_set1_epi32 ((int)map->tint[k])), 6);
					c = _mm512_and_si512 (_mm512_srl_epi32 (pal, shift), channel);
					c = _mm512_srli_epi32 (_mm512_mullo_epi32 (c, l), 15);
					f = _mm512_and_si512 (_mm512_srl_epi32 (fl, shift), channel);
					c = _mm512_min_epu32 (_mm512_max_epu32 (c, f), channel);
					color = _mm512_or_si512 (color, _mm512_sll_epi32 (c, shift));
				}
			}
			_mm512_mask_storeu_epi32 (dest + i, visible, color);
			_mm512_mask_storeu_ps (zbuf + i, visible, z);
		}

		// sixteen pixels on
		zi = (int)((unsigned)zi + 16u * (unsigned)map->zistep);
		light = (int)((unsigned)light + 16u * (unsigned)map->lightstep);
		snext = sfrac + 16 * map->sfracstep;
		tnext = tfrac + 16 * map->tfracstep;
		tex += 16 * map->stepwhole + (snext >> 16) + (tnext >> 16) * map->skinwidth;
		sfrac = snext & 0xFFFF;
		tfrac = tnext & 0xFFFF;
	}
}

// for each scale, the source lane of each output lane: (chunk * 16 + lane) / scale
static __m512i	expand_index[17][16];
static bool		expand_ready;

static void Simd_V4_ExpandTables (void)
{
	int		scale, chunk, lane;
	int32_t	idx[16];

	for (scale = 1 ; scale <= 16 ; scale++)
		for (chunk = 0 ; chunk < scale ; chunk++)
		{
			for (lane = 0 ; lane < 16 ; lane++)
				idx[lane] = (chunk * 16 + lane) / scale;
			expand_index[scale][chunk] = _mm512_loadu_si512 (idx);
		}
	expand_ready = true;
}

// one 10 bit channel of src over dest at bit shift, in linear light as
// simd_scalar.c blends it
static inline __m512i Simd_V4_BlendChannel (__m512i s, __m512i d, __m512 a, __m512 ia, int shift)
{
	const __m512i	mask = _mm512_set1_epi32 (1023);
	__m512			fs = _mm512_cvtepi32_ps (_mm512_and_si512 (_mm512_srli_epi32 (s, (unsigned)shift), mask));
	__m512			fd = _mm512_cvtepi32_ps (_mm512_and_si512 (_mm512_srli_epi32 (d, (unsigned)shift), mask));
	__m512			x;
	__m512i			c;

	fs = _mm512_mul_ps (fs, fs);
	fs = _mm512_mul_ps (fs, fs);
	fd = _mm512_mul_ps (fd, fd);
	fd = _mm512_mul_ps (fd, fd);
	x = _mm512_mul_ps (_mm512_add_ps (_mm512_mul_ps (fs, a), _mm512_mul_ps (fd, ia)), _mm512_set1_ps (1.0f / 256.0f));
	c = _mm512_cvttps_epi32 (_mm512_add_ps (_mm512_sqrt_ps (_mm512_sqrt_ps (x)), _mm512_set1_ps (0.5f)));
	return _mm512_slli_epi32 (_mm512_min_epi32 (c, mask), (unsigned)shift);
}

void Simd_V4_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
	int alpha, int count)
{
	const __m512	lane = _mm512_cvtepi32_ps (Simd_V4_Iota ());
	const __m512	vzi = _mm512_set1_ps (zi);
	const __m512	vstep = _mm512_set1_ps (step);
	const __m512	a = _mm512_set1_ps ((float)alpha);
	const __m512	ia = _mm512_set1_ps ((float)(256 - alpha));
	const __m512i	cutout = _mm512_set1_epi32 ((int)0x80000000u);
	__m512i			s, d, out;
	__mmask16		m;
	int				i;

	for (i = 0 ; i < count ; i += 16)
	{
		m = Simd_V4_Lanes (count - i);
		s = _mm512_maskz_loadu_epi32 (m, src + i);
		m &= (__mmask16)~_mm512_test_epi32_mask (s, cutout);
		if (zbuf)
		{
			__m512	idx = _mm512_add_ps (_mm512_set1_ps ((float)i), lane);	// whole numbers, exact
			__m512	z = _mm512_add_ps (vzi, _mm512_mul_ps (idx, vstep));

			m = _mm512_mask_cmp_ps_mask (m, _mm512_maskz_loadu_ps (m, zbuf + i), z, _CMP_LE_OQ);
		}
		if (!m)
			continue;
		d = _mm512_maskz_loadu_epi32 (m, dest + i);
		out = _mm512_or_si512 (Simd_V4_BlendChannel (s, d, a, ia, 0),
			_mm512_or_si512 (Simd_V4_BlendChannel (s, d, a, ia, 10), Simd_V4_BlendChannel (s, d, a, ia, 20)));
		_mm512_mask_storeu_epi32 (dest + i, m, out);
	}
}

void Simd_V4_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	__m512i		tex, colors, keep, idx;
	__mmask16	lanes, kept, written;
	int			i, n, chunk, outcount;

	if (scale < 1 || scale > 16)
	{
		Simd_Scalar_Expand8 (dest, src, palette, count, scale, transparent);
		return;
	}
	if (!expand_ready)
		Simd_V4_ExpandTables ();

	for (i = 0 ; i < count ; i += 16)
	{
		n = count - i < 16 ? count - i : 16;
		lanes = Simd_V4_Lanes (n);
		tex = Simd_V4_LoadTexels (src + i, lanes);
		colors = _mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, tex, palette, 4);
		kept = lanes & ~_mm512_cmpeq_epi32_mask (tex, _mm512_set1_epi32 (transparent));
		keep = _mm512_maskz_set1_epi32 (kept, -1);

		outcount = n * scale;
		for (chunk = 0 ; chunk * 16 < outcount ; chunk++)
		{
			idx = expand_index[scale][chunk];
			written = _mm512_test_epi32_mask (_mm512_permutexvar_epi32 (idx, keep), _mm512_permutexvar_epi32 (idx, keep));
			_mm512_mask_storeu_epi32 (dest + (size_t)i * scale + chunk * 16,
				Simd_V4_Lanes (outcount - chunk * 16) & written, _mm512_permutexvar_epi32 (idx, colors));
		}
	}
}

void Simd_V4_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
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

		// streaming stores need 64 byte aligned destinations
		head = (64 - ((uintptr_t)d & 63)) & 63;
		if (head > bytes)
			head = bytes;
		memcpy (d, s, head);
		d += head;
		s += head;
		bytes -= head;

		for ( ; bytes >= 64 ; bytes -= 64, d += 64, s += 64)
			_mm512_stream_si512 ((void *)d, _mm512_loadu_si512 (s));
		memcpy (d, s, bytes);
	}
	_mm_sfence ();
}

// fog is the scalar kernel's until it is vectorized here
void Simd_V4_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
	const simd_fog_t *fog)
{
	Simd_Scalar_FogSpan (dest, zbuf, zi, step, count, fog);
}
