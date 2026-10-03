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
// test_present_webgl.c -- the web's present.glsl drawn with WebGL 2 as vid_webgl.c
// draws it, from layers laid out as the renderer fills them, into a target of its
// own read back (vid_webgl.js's web_vid_drawto), in a browser (emrun): each pixel
// within 1 of what VID_FrameToRGB (screenshots) makes of it, with and without gamma
// and a view blend; and scaled by 2.5, sharp bilinear, each pixel inside a texel
// that texel's, and each on an edge between its two. Without WebGL 2 it is skipped.

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

extern const char	vid_present_glsl[];	// present_glsl.c

// vid_webgl.js
bool	web_vid_init (const char *source, bool desync);
void	web_vid_error (char *buf, int size);
void	web_vid_renderer (char *buf, int size);
bool	web_vid_settextures (int width, int height);
int		web_vid_drawto (const pixel_t *view, const hudpixel_t *hud, unsigned rowpixels,
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

static void Draw (int width, int height, float *out)
{
	vid_fit_t				fit = {0, 0, (float)width, (float)height, (float)width / WIDTH, 1};
	vid_present_constants_t	constants;

	VID_FillConstants (&constants, &fit, VID_OUTPUT_SDR, 1, 1);
	web_vid_drawto (frame, hud, vid.rowpixels, &constants, width, height, out);
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

	Draw (WIDTH, HEIGHT, out);
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

	Draw (WIDTH, HEIGHT, one);
	Draw (MAX_WIDTH, MAX_HEIGHT, scaled);
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

int main (void)
{
	char	text[256];

	VID_RegisterCommon ();
	vid.width = WIDTH;
	vid.height = HEIGHT;
	vid.rowpixels = WIDTH;

	if (!web_vid_init (vid_present_glsl, false))
	{
		web_vid_error (text, sizeof(text));
		printf ("present_webgl: no WebGL 2 (%s), skipped\n", text);
		return SKIPPED;
	}
	web_vid_renderer (text, sizeof(text));
	printf ("present_webgl: %s\n", text);
	if (!web_vid_settextures (WIDTH, HEIGHT))
	{
		web_vid_error (text, sizeof(text));
		Sys_Error ("%s", text);
	}

	Fill ();
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	TestSDR ("SDR");
	TestSharp ();

	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	TestSDR ("SDR, gamma 0.8 and a blend");

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present_webgl: the shader shows what screenshots do\n");
	return 0;
}
