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
//
// And the CRT shader's passes after the frame, as vid_webgl.c has them drawn
// (web_vid_crt_drawto): with its own oimage 1 each pixel is the frame's as drawn
// at its own size, within 1 of VID_FrameToRGB; at its defaults, three frames of a
// frame white in its top left quarter are finite and brightest there.

#include "cvar.h"
#include "mem.h"
#include "sys.h"
#include "vid_common.h"
#include "vid_crt.h"

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
extern const char	vid_crt_glsl[], vid_crt_core[], vid_crt_params[];	// crt_glsl.c

// vid_webgl.js
bool	web_vid_init (const char *source, bool desync);
void	web_vid_error (char *buf, int size);
void	web_vid_renderer (char *buf, int size);
bool	web_vid_settextures (int width, int height);
int		web_vid_drawto (const pixel_t *view, const hudpixel_t *hud, unsigned rowpixels,
			const vid_present_constants_t *constants, int width, int height, float *out);
bool	web_vid_crt_init (const char *glsl, const char *core, const char *params, const char *names, int mipspass,
			int paramssize);
void	web_vid_crt_texture (int texture, int width, int height, bool half, bool linear, int levels, bool feedback,
			const byte *pixels);
int		web_vid_crt_drawto (const pixel_t *view, const hudpixel_t *hud, unsigned rowpixels,
			const vid_present_constants_t *constants, const byte *params, const vid_crt_pass_t *passes,
			const int *table, int width, int height, float *out);

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

/*
===============================================================================

THE CRT SHADER

===============================================================================
*/

// its passes made, as vid_webgl.c's VID_SetCRT makes them
static bool InitCRT (void)
{
	char	names[256] = "";
	byte	*pixels;
	int		i, w, h;

	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		if (i)
			strcat (names, ",");
		strcat (names, vid_crt_passes[i].name);
	}
	if (!web_vid_crt_init (vid_crt_glsl, vid_crt_core, vid_crt_params, names, CRT_PASS_PREPASS,
			(int)VID_CRT_PARAMS_SIZE))
		return false;
	for (i = 0 ; i < 4 ; i++)
	{
		pixels = VID_CRT_LoadLUT (i, &w, &h);
		if (!pixels)
			return false;
		web_vid_crt_texture (CRT_LUT1 + i, w, h, false, true, 1, false, pixels);
		Mem_Free (pixels);
	}
	return true;
}

// the frame through the passes into width x height, as vid_webgl.c's
// VID_PresentCRT has them drawn, a frame of 60 Hz after the last
static void DrawCRT (const vid_crt_params_t *params, int width, int height, float *out)
{
	static vid_crt_frame_t		crtframe;
	static double				time = 1;
	static byte					block[VID_CRT_PARAMS_SIZE];
	const vid_crt_texture_t		*t;
	const vid_crt_pass_info_t	*pass;
	vid_fit_t					whole = {0, 0, WIDTH, HEIGHT, 1, 1};
	vid_present_constants_t		constants;
	vid_crt_pass_t				passes[CRT_PASSES];
	int							table[CRT_PASSES][8], i, j, w, h;

	for (i = 0 ; i < CRT_TEXTURES ; i++)
	{
		t = &vid_crt_textures[i];
		if (t->size == CRT_SIZE_LUT)
			continue;
		VID_CRT_TextureSize (i, WIDTH, HEIGHT, width, height, &w, &h);
		web_vid_crt_texture (i, w, h, t->format == CRT_RGBA16F, t->linear, t->mips ? VID_CRT_MipLevels (w, h) : 1,
			t->feedback, NULL);
	}
	VID_FillConstants (&constants, &whole, VID_OUTPUT_SDR, 1, 1);
	time += 1 / 60.0;
	VID_CRT_BeginFrame (&crtframe, time);
	memcpy (block, params, sizeof(*params));
	VID_CRT_FillPasses (passes, &crtframe, WIDTH, HEIGHT, width, height, VID_OUTPUT_SDR, 1);
	for (i = 0 ; i < CRT_PASSES ; i++)
	{
		pass = &vid_crt_passes[i];
		table[i][0] = pass->output == CRT_SCREEN ? -1 : VID_CRT_Physical (pass->output, crtframe.parity);
		for (j = 0 ; j < CRT_MAX_INPUTS ; j++)
			table[i][j + 1] = j < pass->numinputs ? VID_CRT_Physical (pass->inputs[j], crtframe.parity) : -1;
	}
	web_vid_crt_drawto (frame, hud, vid.rowpixels, &constants, block, passes, &table[0][0], width, height, out);
}

// with oimage 1, the frame as it is, within 1 of VID_FrameToRGB
static void TestCRTOriginal (void)
{
	static float		out[WIDTH * HEIGHT * 4];
	static byte			rgb[WIDTH * HEIGHT * 3];
	vid_crt_params_t	params;
	int					i, c, d, worst = 0, off = 0;

	VID_CRT_FillParams (&params);
	params.oimage = 1;
	Fill ();
	VID_SetPresent (&(vid_present_t){.blend = {0.5f, 0.2f, 0.1f, 0.3f}, .gamma = 0.8f});
	DrawCRT (&params, WIDTH, HEIGHT, out);
	VID_FrameToRGB (rgb, true);
	for (i = 0 ; i < WIDTH * HEIGHT ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			d = abs ((int)lroundf (out[i * 4 + c] * 255) - rgb[i * 3 + c]);
			if (d > worst)
				worst = d;
			if (d > 1 && off++ < 5)
				printf ("CRT original: pixel %d channel %d is %.1f, VID_FrameToRGB %d\n", i, c, out[i * 4 + c] * 255,
					rgb[i * 3 + c]);
		}
	VID_SetPresent (&(vid_present_t){.gamma = 1});
	if (off)
		failures++;
	printf ("CRT, the original image: at most %d from VID_FrameToRGB, %d channels over 1\n", worst, off);
}

// at the defaults, a frame white in its top left quarter: three frames, every
// value finite, and the quarter four times as bright as the one across from it
static void TestCRTDefaults (void)
{
	static float		out[MAX_WIDTH * MAX_HEIGHT * 4];
	vid_crt_params_t	params;
	double				topleft = 0, bottomright = 0;
	float				v;
	int					x, y, i, bad = 0;

	VID_CRT_FillParams (&params);
	memset (hud, 0, sizeof(hud));
	for (y = 0 ; y < HEIGHT ; y++)
		for (x = 0 ; x < WIDTH ; x++)
			frame[y * vid.rowpixels + x] = x < WIDTH / 2 && y < HEIGHT / 2
				? RGB30 (RGB30_WHITE, RGB30_WHITE, RGB30_WHITE) : RGB30 (0, 0, 0);
	for (i = 0 ; i < 3 ; i++)
		DrawCRT (&params, MAX_WIDTH, MAX_HEIGHT, out);
	for (y = 0 ; y < MAX_HEIGHT ; y++)
		for (x = 0 ; x < MAX_WIDTH ; x++)
			for (i = 0 ; i < 3 ; i++)
			{
				v = out[(y * MAX_WIDTH + x) * 4 + i];
				if (!isfinite (v) || v < 0 || v > 1.001f)
				{
					if (bad++ < 5)
						printf ("CRT defaults: pixel %d,%d channel %d is %f\n", x, y, i, v);
					continue;
				}
				if (x < MAX_WIDTH / 2 && y < MAX_HEIGHT / 2)
					topleft += v;
				else if (x >= MAX_WIDTH / 2 && y >= MAX_HEIGHT / 2)
					bottomright += v;
			}
	topleft /= MAX_WIDTH / 2 * MAX_HEIGHT / 2 * 3;
	bottomright /= MAX_WIDTH / 2 * MAX_HEIGHT / 2 * 3;
	printf ("CRT, defaults, 3 frames: %d values not in 0 to 1, the white quarter %.3f, the black one %.3f\n", bad,
		topleft, bottomright);
	// the glow and bloom light the black quarter a little
	if (bad || topleft < 0.3 || bottomright > topleft / 4)
		failures++;
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

	if (!InitCRT ())
	{
		web_vid_error (text, sizeof(text));
		printf ("CRT: the shader's passes couldn't be made: %s\n", text);
		return 1;
	}
	TestCRTOriginal ();
	TestCRTDefaults ();

	if (failures)
	{
		printf ("%d failures\n", failures);
		return 1;
	}
	printf ("present_webgl: the shader shows what screenshots do\n");
	return 0;
}
