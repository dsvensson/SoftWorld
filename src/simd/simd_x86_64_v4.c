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

void Simd_V4_TexSpan (uint32_t *dest, const uint32_t *src, int srcwidth,
	int s, int t, int sstep, int tstep, int count)
{
	__mmask16	lanes = Simd_V4_Lanes (count);
	__m512i		lane = Simd_V4_Iota ();
	__m512i		vs, vt, idx;

	vs = _mm512_add_epi32 (_mm512_set1_epi32 (s), _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (sstep)));
	vt = _mm512_add_epi32 (_mm512_set1_epi32 (t), _mm512_mullo_epi32 (lane, _mm512_set1_epi32 (tstep)));
	idx = _mm512_add_epi32 (_mm512_srai_epi32 (vs, 16),
		_mm512_mullo_epi32 (_mm512_srai_epi32 (vt, 16), _mm512_set1_epi32 (srcwidth)));
	_mm512_mask_storeu_epi32 (dest, lanes,
		_mm512_mask_i32gather_epi32 (_mm512_setzero_si512 (), lanes, idx, src, 4));
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
