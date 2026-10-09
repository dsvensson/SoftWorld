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
// r_image.c -- images from files as pixels of the 3D view (vid.h): sRGB
// texels made RGB30, and scaled by averaging in linear light

#include "r_local.h"
#include "tga.h"

static bool		r_imagetables;
static unsigned	r_srgbcode[256];	// an sRGB byte as a pixel's channel
static float	r_srgblight[256];	// that channel to the fourth: linear light

// before any loader's thread starts (R_InitPalette); until then on first use
void R_InitImageTables (void)
{
	int		i;

	for (i = 0 ; i < 256 ; i++)
	{
		r_srgbcode[i] = R_LightCode (R_SrgbToLinear (i / 255.0));
		r_srgblight[i] = (float)r_srgbcode[i] * (float)r_srgbcode[i];
		r_srgblight[i] *= r_srgblight[i];
	}
	r_imagetables = true;
}

const float *R_SrgbLightTable (void)
{
	if (!r_imagetables)
		R_InitImageTables ();
	return r_srgblight;
}

/*
===============
R_LoadTGA

An image file as RGBA, top row first, from Mem_Alloc; NULL if there is no
such file, or (said on the console) it isn't a TGA file that can be read
===============
*/
byte *R_LoadTGA (const char *path, int *width, int *height)
{
	byte		*file, *rgba;
	const char	*error;
	int			len;

	file = FS_LoadFile (path, &len);
	if (!file)
		return NULL;
	rgba = TGA_Decode (file, len, width, height, &error);
	Mem_Free (file);
	if (!rgba)
		Con_Printf ("%s: %s\n", path, error);
	return rgba;
}

/*
===============
R_RGBA8Pixel

An sRGB texel as a pixel; alpha is left out
===============
*/
pixel_t R_RGBA8Pixel (const byte *rgba)
{
	if (!r_imagetables)
		R_InitImageTables ();
	return RGB30 (r_srgbcode[rgba[0]], r_srgbcode[rgba[1]], r_srgbcode[rgba[2]]);
}

// linear light, as r_srgblight holds it, as a pixel's channel
static unsigned R_LightChannel (float light)
{
	unsigned	c = (unsigned)(sqrtf (sqrtf (light)) + 0.5f);

	return c < 1023 ? c : 1023;
}

/*
===============
R_AveragePixels

n pixels averaged in linear light; their top bits are left out
===============
*/
pixel_t R_AveragePixels (const pixel_t *p, int n)
{
	float	sum[3] = {0, 0, 0}, v;
	int		i, c;

	if (n <= 0)
		return 0;
	for (i = 0 ; i < n ; i++)
		for (c = 0 ; c < 3 ; c++)
		{
			v = (float)((p[i] >> (10 * c)) & 1023);
			v *= v;
			sum[c] += v * v;
		}
	return RGB30 (R_LightChannel (sum[0] / (float)n), R_LightChannel (sum[1] / (float)n),
		R_LightChannel (sum[2] / (float)n));
}

/*
===============
R_ImagePixels

A w x h RGBA image as outw x outh pixels, each the average in linear light of
the texels it covers, or the one it falls on where the image is smaller. With
cutout, the texels count as much as they are opaque, and a pixel of texels
less than half opaque on average is cut out (PIXEL_TRANSPARENT); without,
alpha is left out.
===============
*/
void R_ImagePixels (const byte *rgba, int w, int h, pixel_t *out, int outw, int outh, bool cutout)
{
	int			x, y, sx, sy, x0, x1, y0, y1, n;
	float		sum[3], weight, alpha, a;
	const byte	*texel;
	pixel_t		p;

	if (!r_imagetables)
		R_InitImageTables ();

	for (y = 0 ; y < outh ; y++)
	{
		y0 = (int)((int64_t)y * h / outh);
		y1 = (int)((int64_t)(y + 1) * h / outh);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0 ; x < outw ; x++)
		{
			x0 = (int)((int64_t)x * w / outw);
			x1 = (int)((int64_t)(x + 1) * w / outw);
			if (x1 <= x0)
				x1 = x0 + 1;

			if (x1 - x0 == 1 && y1 - y0 == 1)
			{
				texel = rgba + ((size_t)y0 * w + x0) * 4;
				p = R_RGBA8Pixel (texel);
				if (cutout && texel[3] < 128)
					p |= PIXEL_TRANSPARENT;
				out[y * outw + x] = p;
				continue;
			}

			sum[0] = sum[1] = sum[2] = 0;
			weight = alpha = 0;
			n = 0;
			for (sy = y0 ; sy < y1 ; sy++)
			{
				texel = rgba + ((size_t)sy * w + x0) * 4;
				for (sx = x0 ; sx < x1 ; sx++, texel += 4)
				{
					a = cutout ? (float)texel[3] : 1.0f;
					sum[0] += r_srgblight[texel[0]] * a;
					sum[1] += r_srgblight[texel[1]] * a;
					sum[2] += r_srgblight[texel[2]] * a;
					weight += a;
					alpha += (float)texel[3];
					n++;
				}
			}
			if (weight > 0)
				p = RGB30 (R_LightChannel (sum[0] / weight), R_LightChannel (sum[1] / weight),
					R_LightChannel (sum[2] / weight));
			else
				p = 0;
			if (cutout && alpha < 128.0f * (float)n)
				p |= PIXEL_TRANSPARENT;
			out[y * outw + x] = p;
		}
	}
}
