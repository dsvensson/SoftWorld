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
// test_simd.c -- each x86-64-v4 kernel against the scalar reference, on
// random input; the results must be the same bit for bit

#include "simd_backends.h"

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
		Simd_V4_ZSpan (b, count, zi, step);
		Check ("ZSpan", r, !memcmp (a, b, sizeof(a)));
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
		Simd_V4_TexSpan (b, &map, tex, width, u, v, count);
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
		Simd_V4_TurbSpan (b, &map, tex, palette, turb + (r & 127), u, v, count);
		Check ("TurbSpan", r, !memcmp (a, b, sizeof(a)));
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
		Simd_V4_LitRowColormap (b, src, colormap, l0, (l1 - l0) / count, count);
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
		Simd_V4_LitRowRGB (b, src, palette, floor, light, step, count);
		Check ("LitRowRGB", r, !memcmp (a, b, sizeof(a)));
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
		Simd_V4_Expand8 (b, src, palette, count, scale, transparent);
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
		Simd_V4_CopyStream (b + to, (size_t)destpitch, src + from, (size_t)srcpitch, (size_t)bytes, rows);
		Check ("CopyStream", r, !memcmp (a, b, sizeof(a)));
	}
}

int main (void)
{
	TestZSpan ();
	TestTexSpan ();
	TestTurbSpan ();
	TestLitRowColormap ();
	TestLitRowRGB ();
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
