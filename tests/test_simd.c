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
// test_simd.c -- each kernel of the build's SIMD backend (x86-64-v4, x86-64-v3
// or apple-m3) against the scalar reference, on random input; the results must
// be the same bit for bit

#include "simd_backends.h"

// the backend's kernel, by the prefix the build names (SW_SIMD_PREFIX)
#define TESTED_NAME(prefix, kernel)		prefix##kernel
#define TESTED_EXPAND(prefix, kernel)	TESTED_NAME (prefix, kernel)
#define TESTED(kernel)					TESTED_EXPAND (SW_SIMD_PREFIX, kernel)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUNDS	20000

static uint32_t	rng = 0x2545F491;
static int		failures;

static uint32_t Rand (void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static int RandRange (int lo, int hi)	// lo .. hi inclusive
{
	return lo + (int)(Rand () % (uint32_t)(hi - lo + 1));
}

static float RandFloat (float lo, float hi)
{
	return lo + (hi - lo) * (float)(Rand () & 0xFFFFFF) / (float)0x1000000;
}

static void Check (const char *kernel, int round, bool same)
{
	if (same)
		return;
	if (failures++ < 20)
		printf ("%s differs in round %d\n", kernel, round);
}

static void TestZSpan (void)
{
	float	a[300], b[300];
	int		r, count;

	for (r = 0 ; r < ROUNDS ; r++)
	{
		float	zi = RandFloat (0, 1), step = RandFloat (-1e-4f, 1e-4f);

		count = RandRange (0, 300);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_ZSpan (a, count, zi, step);
		TESTED (ZSpan) (b, count, zi, step);
		Check ("ZSpan", r, !memcmp (a, b, sizeof(a)));
	}
}

// a span blended over another, with or without a depth test; the z buffer
// near the span's own 1/z, so both sides of the test come up
static void TestBlendSpan (void)
{
	uint32_t	dest[300], a[300], b[300], src[300];
	float		zbuf[300];
	int			r, i, count, alpha;

	for (r = 0 ; r < ROUNDS ; r++)
	{
		float	zi = RandFloat (0.01f, 1), step = RandFloat (-1e-4f, 1e-4f);

		count = RandRange (0, 300);
		alpha = RandRange (0, 256);
		for (i = 0 ; i < 300 ; i++)
		{
			dest[i] = Rand () & 0x3FFFFFFF;
			src[i] = Rand () & ((Rand () & 7) ? 0x3FFFFFFFu : 0xFFFFFFFFu);
			zbuf[i] = zi + (float)i * step + RandFloat (-2e-4f, 2e-4f) * (float)(Rand () & 1);
		}
		memcpy (a, dest, sizeof(a));
		memcpy (b, dest, sizeof(b));
		Simd_Scalar_BlendSpan (a, src, (r & 1) ? zbuf : NULL, zi, step, alpha, count);
		TESTED (BlendSpan) (b, src, (r & 1) ? zbuf : NULL, zi, step, alpha, count);
		Check ("BlendSpan", r, !memcmp (a, b, sizeof(a)));
	}
}

// fog over a span: pixels kept (top bit), sky (bit 30) and the rest, by a z
// buffer or a stepped 1/z, some infinitely far (0), some past the table
static void TestFogSpan (void)
{
	uint32_t	dest[300], a[300], b[300];
	float		zbuf[300], table[97];
	simd_fog_t	fog;
	int			r, i, count;

	for (r = 0 ; r < ROUNDS ; r++)
	{
		float	zi = RandFloat (0.0001f, 0.05f), step = RandFloat (-1e-4f, 1e-4f);

		count = RandRange (0, 300);
		fog.size = RandRange (1, 97);
		for (i = 0 ; i < fog.size ; i++)
			table[i] = RandFloat (0, 256);
		fog.table = table;
		{
			// the table over some of the 1/z the pixels have
			float	near = RandFloat (0.0001f, 0.05f);
			int32_t	bits;

			memcpy (&bits, &near, sizeof(bits));
			fog.base = (bits >> SIMD_FOG_SHIFT) - RandRange (0, fog.size);
		}
		for (i = 0 ; i < 3 ; i++)
		{
			fog.color[i] = RandFloat (0, 1023);
			fog.color[i] *= fog.color[i];
			fog.color[i] *= fog.color[i];
		}
		fog.sky = RandFloat (0, 256);
		for (i = 0 ; i < 300 ; i++)
		{
			dest[i] = Rand () & 0x3FFFFFFF;
			if (!(Rand () & 7))
				dest[i] |= 0x80000000u;
			if (!(Rand () & 7))
				dest[i] |= 0x40000000u;
			zbuf[i] = (Rand () & 15) ? RandFloat (0, 0.05f) : (Rand () & 1) ? 0 : -RandFloat (0, 0.05f);
		}
		memcpy (a, dest, sizeof(a));
		memcpy (b, dest, sizeof(b));
		Simd_Scalar_FogSpan (a, (r & 1) ? zbuf : NULL, zi, step, count, &fog);
		TESTED (FogSpan) (b, (r & 1) ? zbuf : NULL, zi, step, count, &fog);
		Check ("FogSpan", r, !memcmp (a, b, sizeof(a)));
	}
}

// a texture on a random plane, seen from a span that stays in front of the
// viewer: 1/z at least 0.002 over u, v below 2048
static simd_texmap_t RandTexmap (int width, int height)
{
	simd_texmap_t	map;

	map.ziorigin = RandFloat (0.01f, 1.0f);
	map.zistepu = RandFloat (-2e-6f, 2e-6f);
	map.zistepv = RandFloat (-2e-6f, 2e-6f);
	map.sdivzorigin = RandFloat (-1.0f, 1.0f) * map.ziorigin;
	map.sdivzstepu = RandFloat (-1e-4f, 1e-4f);
	map.sdivzstepv = RandFloat (-1e-4f, 1e-4f);
	map.tdivzorigin = RandFloat (-1.0f, 1.0f) * map.ziorigin;
	map.tdivzstepu = RandFloat (-1e-4f, 1e-4f);
	map.tdivzstepv = RandFloat (-1e-4f, 1e-4f);
	map.sadjust = RandRange (-(width << 16), width << 16);
	map.tadjust = RandRange (-(height << 16), height << 16);
	map.sextent = (width << 16) - 1;
	map.textent = (height << 16) - 1;
	return map;
}

static void TestTexSpan (void)
{
	static uint32_t	tex[256 * 256], a[600], b[600];
	simd_texmap_t	map;
	int				r, i, width, height, count, u, v;

	for (i = 0 ; i < 256 * 256 ; i++)
		tex[i] = Rand ();
	for (r = 0 ; r < ROUNDS ; r++)
	{
		width = RandRange (1, 256);
		height = RandRange (1, 256);
		map = RandTexmap (width, height);
		count = RandRange (1, 600);
		u = RandRange (0, 2047 - count);
		v = RandRange (0, 2047);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_TexSpan (a, &map, tex, width, u, v, count);
		TESTED (TexSpan) (b, &map, tex, width, u, v, count);
		Check ("TexSpan", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestTurbSpan (void)
{
	static byte		tex[64 * 64 + 3];
	static uint32_t	palette[256], a[600], b[600];
	static int		turb[256];
	simd_texmap_t	map;
	int				r, i, count, u, v;

	for (i = 0 ; i < (int)sizeof(tex) ; i++)
		tex[i] = (byte)Rand ();
	for (i = 0 ; i < 256 ; i++)
	{
		palette[i] = Rand ();
		turb[i] = RandRange (0, 16 << 16);
	}
	for (r = 0 ; r < ROUNDS ; r++)
	{
		map = RandTexmap (RandRange (1, 1024), RandRange (1, 1024));
		count = RandRange (1, 600);
		u = RandRange (0, 2047 - count);
		v = RandRange (0, 2047);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_TurbSpan (a, &map, tex, palette, turb + (r & 127), u, v, count);
		TESTED (TurbSpan) (b, &map, tex, palette, turb + (r & 127), u, v, count);
		Check ("TurbSpan", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestTurbSpanRGB30 (void)
{
	static uint32_t	tex[64 * 64], a[600], b[600];
	static int		turb[256];
	simd_texmap_t	map;
	int				r, i, count, u, v;

	for (i = 0 ; i < 64 * 64 ; i++)
		tex[i] = Rand ();
	for (i = 0 ; i < 256 ; i++)
		turb[i] = RandRange (0, 16 << 16);
	for (r = 0 ; r < ROUNDS ; r++)
	{
		map = RandTexmap (RandRange (1, 1024), RandRange (1, 1024));
		count = RandRange (1, 600);
		u = RandRange (0, 2047 - count);
		v = RandRange (0, 2047);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_TurbSpanRGB30 (a, &map, tex, turb + (r & 127), u, v, count);
		TESTED (TurbSpanRGB30) (b, &map, tex, turb + (r & 127), u, v, count);
		Check ("TurbSpanRGB30", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestLitRowColormap (void)
{
	static uint32_t	colormap[64 * 256];
	byte			src[16];
	uint32_t		a[16], b[16];
	int				r, i, count, l0, l1;

	for (i = 0 ; i < 64 * 256 ; i++)
		colormap[i] = Rand ();
	for (r = 0 ; r < ROUNDS ; r++)
	{
		count = 1 << RandRange (1, 4);
		for (i = 0 ; i < 16 ; i++)
			src[i] = (byte)Rand ();
		l0 = RandRange (0, 0x3FFF);
		l1 = RandRange (0, 0x3FFF);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_LitRowColormap (a, src, colormap, l0, (l1 - l0) / count, count);
		TESTED (LitRowColormap) (b, src, colormap, l0, (l1 - l0) / count, count);
		Check ("LitRowColormap", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestLitRowRGB (void)
{
	uint32_t	palette[256], floor[256], a[16], b[16];
	byte		src[16];
	int			r, i, k, count, light[3], step[3];

	for (i = 0 ; i < 256 ; i++)
	{
		palette[i] = (Rand () & 255) | ((Rand () & 255) << 10) | ((Rand () & 255) << 20);
		floor[i] = (Rand () & 3) ? 0 : ((Rand () & 1023) | ((Rand () & 1023) << 10) | ((Rand () & 1023) << 20));
	}
	for (r = 0 ; r < ROUNDS ; r++)
	{
		count = 1 << RandRange (1, 4);
		for (i = 0 ; i < 16 ; i++)
			src[i] = (byte)Rand ();
		for (k = 0 ; k < 3 ; k++)
		{
			light[k] = RandRange (-4096, 4 * 32768);
			step[k] = RandRange (-16384, 16384);
		}
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_LitRowRGB (a, src, palette, floor, light, step, count);
		TESTED (LitRowRGB) (b, src, palette, floor, light, step, count);
		Check ("LitRowRGB", r, !memcmp (a, b, sizeof(a)));
	}
}

// truecolor texels, some cut out, lit with and without glow, rows of 2 to 16
// and odd lengths
static void TestLitRowRGB30 (void)
{
	uint32_t	src[16], glow[16], a[16], b[16];
	int			r, i, k, count, glowscale, light[3], step[3];

	for (r = 0 ; r < ROUNDS ; r++)
	{
		count = (r & 2) ? RandRange (1, 16) : 1 << RandRange (1, 4);
		for (i = 0 ; i < 16 ; i++)
		{
			src[i] = Rand () & ((Rand () & 7) ? 0x3FFFFFFFu : 0xBFFFFFFFu);
			glow[i] = (Rand () & 3) ? 0 : Rand () & 0x3FFFFFFF;
		}
		glowscale = RandRange (0, 2 * 32768);
		for (k = 0 ; k < 3 ; k++)
		{
			light[k] = RandRange (-4096, 4 * 32768);
			step[k] = RandRange (-16384, 16384);
		}
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_LitRowRGB30 (a, src, (r & 1) ? glow : NULL, glowscale, light, step, count);
		TESTED (LitRowRGB30) (b, src, (r & 1) ? glow : NULL, glowscale, light, step, count);
		Check ("LitRowRGB30", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestAliasSpan (void)
{
	static byte		skin[320 * 200 + 8], remap[256 + 8];
	static uint32_t	colormap[64 * 256], palette[256], floor[256];
	static uint32_t	a[400], b[400];
	static float	za[400], zb[400], zinit[400];
	simd_aliasmap_t	map;
	const byte		*tex;
	int				r, i, width, height, count, s0, s1, t0, t1, sstep, tstep, light0, light1, zi;

	for (i = 0 ; i < (int)sizeof(skin) ; i++)
		skin[i] = (byte)Rand ();
	for (i = 0 ; i < (int)sizeof(remap) ; i++)
		remap[i] = (byte)Rand ();
	for (i = 0 ; i < 64 * 256 ; i++)
		colormap[i] = Rand ();
	for (i = 0 ; i < 256 ; i++)
	{
		palette[i] = Rand () & 0x3FFFFFFF;
		floor[i] = (Rand () & 3) ? 0 : Rand () & 0x3FFFFFFF;
	}
	for (r = 0 ; r < ROUNDS ; r++)
	{
		// a span that stays on the skin, as the rasterizer's do
		width = RandRange (8, 320);
		height = RandRange (8, 200);
		count = RandRange (1, 400);
		s0 = RandRange (0, (width << 16) - 1);
		s1 = RandRange (0, (width << 16) - 1);
		t0 = RandRange (0, (height << 16) - 1);
		t1 = RandRange (0, (height << 16) - 1);
		sstep = (s1 - s0) / count;
		tstep = (t1 - t0) / count;
		map.zistep = RandRange (-100000, 100000);
		map.stepwhole = (sstep >> 16) + (tstep >> 16) * width;
		map.sfracstep = sstep & 0xFFFF;
		map.tfracstep = tstep & 0xFFFF;
		map.skinwidth = width;
		map.remap = remap + (r & 7);
		map.colormap = (r & 1) ? colormap : NULL;
		map.palette = palette;
		map.floor = floor;
		for (i = 0 ; i < 3 ; i++)
			map.tint[i] = (unsigned)RandRange (0, 256);
		// classic light must stay on the colormap
		light0 = RandRange (0, 0x3FFF);
		light1 = RandRange (0, 0x3FFF);
		map.lightstep = (light1 - light0) / count;
		tex = skin + (r & 3) + (s0 >> 16) + (t0 >> 16) * width;
		for (i = 0 ; i < 400 ; i++)
			zinit[i] = RandFloat (0, 1);
		zi = RandRange (0, 0x7FFFFFFF - 400 * 100000);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		memcpy (za, zinit, sizeof(za));
		memcpy (zb, zinit, sizeof(zb));
		Simd_Scalar_AliasSpan (a, za, tex, s0 & 0xFFFF, t0 & 0xFFFF, light0, zi, count, &map);
		TESTED (AliasSpan) (b, zb, tex, s0 & 0xFFFF, t0 & 0xFFFF, light0, zi, count, &map);
		Check ("AliasSpan", r, !memcmp (a, b, sizeof(a)) && !memcmp (za, zb, sizeof(za)));
	}
}

static void TestExpand8 (void)
{
	uint32_t	palette[256], a[100 * 16], b[100 * 16];
	byte		src[100];
	int			r, i, count, scale, transparent;

	for (i = 0 ; i < 256 ; i++)
		palette[i] = Rand ();
	for (r = 0 ; r < ROUNDS ; r++)
	{
		count = RandRange (0, 100);
		scale = RandRange (1, 16);
		transparent = (Rand () & 1) ? -1 : (int)(Rand () & 255);
		for (i = 0 ; i < 100 ; i++)
			src[i] = (Rand () & 3) ? (byte)Rand () : (byte)(transparent & 255);
		for (i = 0 ; i < 100 * 16 ; i++)
			a[i] = b[i] = 0xDEADBEEF;
		Simd_Scalar_Expand8 (a, src, palette, count, scale, transparent);
		TESTED (Expand8) (b, src, palette, count, scale, transparent);
		Check ("Expand8", r, !memcmp (a, b, sizeof(a)));
	}
}

static void TestCopyStream (void)
{
	static byte	src[4 * 1100 + 64], a[4 * 1200 + 128], b[4 * 1200 + 128];
	int			r, i, bytes, rows, srcpitch, destpitch, from, to;

	for (i = 0 ; i < (int)sizeof(src) ; i++)
		src[i] = (byte)Rand ();
	for (r = 0 ; r < ROUNDS / 10 ; r++)
	{
		bytes = RandRange (0, 1000);
		srcpitch = bytes + RandRange (0, 100);
		destpitch = bytes + RandRange (0, 200);
		rows = RandRange (0, 4);
		from = RandRange (0, 63);
		to = RandRange (0, 63);
		memset (a, 0, sizeof(a));
		memset (b, 0, sizeof(b));
		Simd_Scalar_CopyStream (a + to, (size_t)destpitch, src + from, (size_t)srcpitch, (size_t)bytes, rows);
		TESTED (CopyStream) (b + to, (size_t)destpitch, src + from, (size_t)srcpitch, (size_t)bytes, rows);
		Check ("CopyStream", r, !memcmp (a, b, sizeof(a)));
	}
}

int main (void)
{
	TestZSpan ();
	TestTexSpan ();
	TestTurbSpan ();
	TestTurbSpanRGB30 ();
	TestLitRowColormap ();
	TestLitRowRGB ();
	TestLitRowRGB30 ();
	TestAliasSpan ();
	TestBlendSpan ();
	TestFogSpan ();
	TestExpand8 ();
	TestCopyStream ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("all kernels match\n");
	return 0;
}
