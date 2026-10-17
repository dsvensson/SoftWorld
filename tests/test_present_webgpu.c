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
// test_present_webgpu.c -- the web's present.wgsl drawn with WebGPU as vid_webgpu.js
// draws it, from layers laid out as the renderer fills them, into a float target of
// its own read back (web_gpu_drawto), in a browser (emrun): for SDR each pixel within
// 1 of what VID_FrameToRGB (screenshots) makes of it, with and without gamma and a
// view blend, and scaled by 2.5, sharp bilinear, each pixel inside a texel that
// texel's, and each on an edge between its two; for HDR, a browser's extended sRGB,
// SDR white at paper white, the brightest light rolled off below the peak, and the 2D
// at paper white, each pixel as VID_FrameToPQ (HDR screenshots). WebGPU reads back
// asynchronously, which the test waits for (Asyncify). Without WebGPU it is skipped.

#include "cvar.h"
#include "sys.h"
#include "vid_common.h"

#include <emscripten/emscripten.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH		200
#define HEIGHT		120
#define SCALED		5				// WIDTH and HEIGHT times this, halved: 2.5
#define MAX_WIDTH	(WIDTH * SCALED / 2)
#define MAX_HEIGHT	(HEIGHT * SCALED / 2)
#define SKIPPED		77

extern const char	vid_present_wgsl[];	// present_wgsl.c

// vid_webgpu.js
bool	web_gpu_start (const char *source);
void	web_gpu_error (char *buf, int size);
void	web_gpu_adapter (char *buf, int size);
bool	web_gpu_settextures (int width, int height);
void	web_gpu_drawto (const pixel_t *view, const hudpixel_t *hud, unsigned rowpixels,
			const vid_present_constants_t *constants, int width, int height, float *out);

static int	failures;

//
// the engine, as far as vid_common.c needs it
//

void Sys_Error (char *error, ...)
{
	va_list	args;

	va_start (args, error);
	printf ("Sys_Error: ");
	vprintf (error, args);
	printf ("\n");
	va_end (args);
	exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	(void)fmt;
}

void R_SetRenderSize (int width, int height, int scale)
{
	(void)width;
	(void)height;
	(void)scale;
}

static pixel_t		frame[WIDTH * HEIGHT];
static hudpixel_t	hud[WIDTH * HEIGHT];

void VID_ShownLayers (const pixel_t **shown, const hudpixel_t **shownhud)
{
	*shown = frame;
	*shownhud = hud;
}

static void Draw (int output, float paperwhite, float peak, int width, int height, float *out)
{
	vid_fit_t				fit = {0, 0, (float)width, (float)height, (float)width / WIDTH, 1};
	vid_present_constants_t	constants;

	VID_FillConstants (&constants, &fit, output, paperwhite, peak);
	web_gpu_drawto (frame, hud, vid.rowpixels, &constants, width, height, out);
}

static uint32_t	rng = 0x2545F491;

static uint32_t Rand (void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// random light, often SDR white and brighter; and random 2D, often none or opaque
static void Fill (void)
{
	unsigned	x, y, a;

	for (y = 0 ; y < HEIGHT ; y++)
		for (x = 0 ; x < WIDTH ; x++)
		{
			frame[y * vid.rowpixels + x] = RGB30 (Rand () % 1024, Rand () % 1024, Rand () % 1024);
			switch (Rand () % 4)
			{
			case 0:		a = 0;					break;
			case 1:		a = 255;				break;
			default:	a = Rand () % 256;		break;
			}
			hud[y * vid.rowpixels + x] = HUD_RGBA (Rand () % (a + 1), Rand () % (a + 1), Rand () % (a + 1), a);
		}
	// the channel order: a red, a green and a blue pixel of SDR white
	frame[0] = RGB30 (RGB30_WHITE, 0, 0);
	frame[1] = RGB30 (0, RGB30_WHITE, 0);
	frame[2] = RGB30 (0, 0, RGB30_WHITE);
	hud[0] = hud[1] = hud[2] = 0;
}

static void TestSDR (const char *what)
{
	static float	out[WIDTH * HEIGHT * 4];
	static byte		rgb[WIDTH * HEIGHT * 3];
	int				i, c, d, worst = 0, off = 0;

	Draw (VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, out);
	VID_FrameToRGB (rgb, true);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			d = abs ((int)lroundf (out[i * 4 + c] * 255) - rgb[i * 3 + c]);
			if (d > worst)
				worst = d;
			if (d > 1 && off++ < 5)
				printf ("%s: pixel %d channel %d is %.1f, VID_FrameToRGB %d\n", what, i, c, out[i * 4 + c] * 255,
					rgb[i * 3 + c]);
		}
	if (off)
		failures++;
	printf ("%s: at most %d from VID_FrameToRGB, %d channels over 1\n", what, worst, off);
}

static void TestSharp (void)
{
	static float	one[WIDTH * HEIGHT * 4], scaled[MAX_WIDTH * MAX_HEIGHT * 4];
	int				x, y, c, tx, ty, off = 0, inside = 0, edges = 0;
	float			fx, fy, got, a, b, lo, hi;

	for (x = 0 ; x < WIDTH * HEIGHT ; x++)
		frame[x] = RGB30 (Rand () % 513, Rand () % 513, Rand () % 513);
	memset (hud, 0, sizeof(hud));

	Draw (VID_OUTPUT_SDR, 1, 1, WIDTH, HEIGHT, one);
	Draw (VID_OUTPUT_SDR, 1, 1, MAX_WIDTH, MAX_HEIGHT, scaled);
	for (y = 0 ; y < MAX_HEIGHT ; y++)
		for (x = 0 ; x < MAX_WIDTH ; x++)
		{
			fx = (x + 0.5f) * WIDTH / MAX_WIDTH;
			fy = (y + 0.5f) * HEIGHT / MAX_HEIGHT;
			tx = (int)fx;
			ty = (int)fy;
			// the blend is 1/2.5 texel wide around each edge: 0.3 of a texel on either side
			if (fabsf (fy - ty - 0.5f) > 0.25f)
				continue;
			for (c = 0 ; c < 3 ; c++)
			{
				got = scaled[(y * MAX_WIDTH + x) * 4 + c];
				a = one[(ty * WIDTH + tx) * 4 + c];
				if (fabsf (fx - tx - 0.5f) <= 0.25f)
				{
					inside += c == 0;
					if (fabsf (got - a) * 255 <= 1)
						continue;
				}
				else
				{
					// between this texel and the one across the nearer edge
					b = one[(ty * WIDTH + (fx - tx < 0.5f ? (tx ? tx - 1 : 0) : (tx < WIDTH - 1 ? tx + 1 : tx))) * 4 + c];
					lo = fminf (a, b) - 1 / 255.0f;
					hi = fmaxf (a, b) + 1 / 255.0f;
					edges += c == 0;
					if (got >= lo && got <= hi)
						continue;
				}
				if (off++ < 5)
					printf ("sharp: pixel %d,%d channel %d is %.1f, texel %d,%d %.1f\n", x, y, c, got * 255, tx, ty,
						a * 255);
			}
		}
	if (off)
		failures++;
	printf ("sharp bilinear: %d pixels inside texels, %d on edges, %d channels off\n", inside, edges, off);
}

// an extended sRGB value's light: sRGB's curve, on past 1
static double ExtendedToLight (double c)
{
	return c <= 0.04045 ? c / 12.92 : pow ((c + 0.055) / 1.055, 2.4);
}

static void Expect (const char *what, float got, float lo, float hi)
{
	if (got >= lo && got <= hi)
		return;
	printf ("%s: %f, not in %f .. %f\n", what, got, lo, hi);
	failures++;
}

static void TestHDR (void)
{
	static float	out[WIDTH * HEIGHT * 4];

	memset (hud, 0, sizeof(hud));
	frame[0] = RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE);
	frame[1] = RGB30 (1023, 1023, 1023);
	frame[2] = RGB30 (1023, 0, 0);
	frame[3] = RGB30 (0, 0, 0);
	hud[3] = HUD_RGBA (255, 255, 255, 255);

	// paper white 1 (a browser's SDR white), peak 6: white is 1, the brightest light (15.9) is under 6, the
	// 2D's white is 1
	Draw (VID_OUTPUT_EXTENDED, 1, 6, WIDTH, HEIGHT, out);
	Expect ("HDR white", (float)ExtendedToLight (out[0]), 0.99f, 1.01f);
	Expect ("HDR brightest", (float)ExtendedToLight (out[4]), 4.5f, 6.0f);
	Expect ("HDR red keeps its hue: green", (float)ExtendedToLight (out[9]), 0, 0.001f);
	Expect ("HDR 2D white", (float)ExtendedToLight (out[12]), 0.99f, 1.01f);
}

// PQ's code to cd/m² (SMPTE ST 2084)
static double PQToNits (double e)
{
	const double	m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double	c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	double			p = pow (e, 1 / m2);

	return 10000 * pow (fmax (p - c1, 0) / (c2 - c3 * p), 1 / m1);
}

/*
================
TestShotHDR

The shader's extended sRGB, as light over paper white at 203 cd/m² in BT.2020,
against what VID_FrameToPQ (HDR screenshots) makes of the same frame, read back
from PQ: within half a percent, or 0.05 cd/m² under 10
================
*/
static void TestShotHDR (float paperwhite, float peak, const char *what)
{
	static const double	bt709to2020[3][3] = {
		{0.627404, 0.329283, 0.043313},
		{0.069097, 0.919541, 0.011362},
		{0.016391, 0.088013, 0.895595}};
	static float	out[WIDTH * HEIGHT * 4];
	static uint16_t	pq[WIDTH * HEIGHT * 3];
	double			l[3], want, got, d, worst = 0;
	int				i, c, off = 0;

	Draw (VID_OUTPUT_EXTENDED, paperwhite, peak, WIDTH, HEIGHT, out);
	VID_FrameToPQ (pq);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
	{
		for (c = 0 ; c < 3 ; c++)
			l[c] = ExtendedToLight (out[i * 4 + c]);
		for (c = 0 ; c < 3 ; c++)
		{
			want = 203 * (bt709to2020[c][0] * l[0] + bt709to2020[c][1] * l[1] + bt709to2020[c][2] * l[2]) / paperwhite;
			got = PQToNits (pq[i * 3 + c] / 65535.0);
			d = fabs (got - want) / fmax (want, 10);
			if (d > worst)
				worst = d;
			if (d > 0.005 && off++ < 5)
				printf ("%s: pixel %d channel %d is %.2f cd/m2, VID_FrameToPQ %.2f\n", what, i, c, want, got);
		}
	}
	if (off)
		failures++;
	printf ("%s: at most %.3f %% from VID_FrameToPQ, %d channels over 0.5 %%\n", what, worst * 100, off);
}

int main (void)
{
	char	text[256];

	VID_RegisterCommon ();
	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = WIDTH;

	if (!web_gpu_start (vid_present_wgsl))
	{
		web_gpu_error (text, sizeof(text));
		printf ("present_webgpu: no WebGPU (%s), skipped\n", text);
		return SKIPPED;
	}
	web_gpu_adapter (text, sizeof(text));
	printf ("present_webgpu: %s\n", text);
	if (!web_gpu_settextures (WIDTH, HEIGHT))
	{
		web_gpu_error (text, sizeof(text));
		Sys_Error ("%s", text);
	}

	Fill ();
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestSDR ("SDR");
	TestSharp ();

	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestSDR ("SDR, gamma 0.8 and a blend");

	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestHDR ();
	Fill ();
	TestShotHDR (1, 1.5f, "HDR screenshot, rolled off below 1.5 (vid_hdr_headroom's default)");
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestShotHDR (1, 6, "HDR screenshot, rolled off below 6, gamma 0.8 and a blend");
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestShotHDR (1, 1, "HDR screenshot, white fitted under a peak at paper white");

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present_webgpu: the shader shows what screenshots do\n");
	return 0;
}
