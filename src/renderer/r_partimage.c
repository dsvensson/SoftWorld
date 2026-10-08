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
// r_partimage.c -- the images scripted particles are drawn with: TGA files,
// or, for names there is no file of, the ones QuakeSpasm-Spiked makes. Texels
// are sRGB with their alpha, as the files have them; each level of detail
// halves the one before, averaging in linear light as much as the texels are
// opaque, down to a texel.

#include "r_local.h"

#define	PART_MAXIMAGES	256

static partimage_t	r_partimages[PART_MAXIMAGES];
static int			r_numpartimages;

// the made ones' names, by fallback (RPI_)
static const char	*r_partfallbacks[RPI_NUMFALLBACKS] = {"*white", "*beam", "*fan", "*ball", "*fuzzy", "*classic"};

// linear light, 0 to 1, as an sRGB byte
static byte R_SrgbByte (float light)
{
	float	c;

	if (light <= 0)
		return 0;
	if (light >= 1)
		return 255;
	c = light <= 0.0031308f ? light * 12.92f : 1.055f * powf (light, 1 / 2.4f) - 0.055f;
	return (byte)(c * 255 + 0.5f);
}

/*
===============
R_PartImageLevels

The image's halvings: each texel of the next level the texels it covers
averaged in linear light, weighted by their alpha, and their alpha averaged
===============
*/
static void R_PartImageLevels (partimage_t *img)
{
	const float	*light = R_SrgbLightTable ();
	const byte	*in, *t;
	byte		*out;
	int			l, w, h, x, y, dx, dy, n, c;
	float		sum[3], alpha, a;

	for (l = 1 ; l < PART_LEVELS && (img->lw[l-1] > 1 || img->lh[l-1] > 1) ; l++)
	{
		in = img->levels[l-1];
		w = img->lw[l] = img->lw[l-1] > 1 ? img->lw[l-1] / 2 : 1;
		h = img->lh[l] = img->lh[l-1] > 1 ? img->lh[l-1] / 2 : 1;
		out = img->levels[l] = Mem_Alloc ((size_t)w * (size_t)h * 4);
		for (y = 0 ; y < h ; y++)
			for (x = 0 ; x < w ; x++)
			{
				sum[0] = sum[1] = sum[2] = alpha = 0;
				n = 0;
				for (dy = 0 ; dy < 2 && y * 2 + dy < img->lh[l-1] ; dy++)
					for (dx = 0 ; dx < 2 && x * 2 + dx < img->lw[l-1] ; dx++)
					{
						t = in + ((y * 2 + dy) * img->lw[l-1] + x * 2 + dx) * 4;
						a = t[3] / 255.0f;
						for (c = 0 ; c < 3 ; c++)
							sum[c] += light[t[c]] * a;
						alpha += a;
						n++;
					}
				for (c = 0 ; c < 3 ; c++)
					out[(y * w + x) * 4 + c] = alpha > 0 ? R_SrgbByte (sum[c] / alpha / PART_WHITE) : 0;
				out[(y * w + x) * 4 + 3] = (byte)(alpha / (float)n * 255 + 0.5f);
			}
	}
	img->numlevels = l;
}

// a new image of its texels, top row first, from Mem_Alloc
static int R_AddPartImage (const char *name, byte *rgba, int width, int height)
{
	partimage_t	*img;

	if (r_numpartimages == PART_MAXIMAGES)
	{
		Mem_Free (rgba);
		return -1;
	}
	img = &r_partimages[r_numpartimages];
	memset (img, 0, sizeof(*img));
	Q_strncpyz (img->name, name, sizeof(img->name));
	img->levels[0] = rgba;
	img->lw[0] = width;
	img->lh[0] = height;
	R_PartImageLevels (img);
	return r_numpartimages++;
}

#define	PART_GENSIZE	64

// QuakeSpasm-Spiked's images for names without a file: alpha over white, or
// for the fan, a corner fading
static int R_MakePartImage (int fallback)
{
	static const byte	fuzzy[16][16] =
	{
		{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
		{0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
		{0,0,0,1,1,1,1,1,3,1,1,2,1,0,0,0},
		{0,0,0,1,1,1,1,4,4,4,5,4,2,1,1,0},
		{0,0,1,1,6,5,5,8,6,8,3,6,3,2,1,0},
		{0,0,1,5,6,7,5,6,8,8,8,3,3,1,0,0},
		{0,0,0,1,6,8,9,9,9,9,4,6,3,1,0,0},
		{0,0,2,1,7,7,9,9,9,9,5,3,1,0,0,0},
		{0,0,2,4,6,8,9,9,9,9,8,6,1,0,0,0},
		{0,0,2,2,3,5,6,8,9,8,8,4,4,1,0,0},
		{0,0,1,2,4,1,8,7,8,8,6,5,4,1,0,0},
		{0,1,1,1,7,8,1,6,7,5,4,7,1,0,0,0},
		{0,1,2,1,1,5,1,3,4,3,1,1,0,0,0,0},
		{0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
	};
	int		x, y, size, a;
	float	dx, dy, d;
	byte	*rgba, *t;

	size = fallback == RPI_WHITE ? 4 : fallback == RPI_FUZZY ? 16 : PART_GENSIZE;
	rgba = Mem_Alloc ((size_t)size * (size_t)size * 4);
	memset (rgba, 0xff, (size_t)size * (size_t)size * 4);
	for (y = 0 ; y < size ; y++)
		for (x = 0 ; x < size ; x++)
		{
			t = rgba + (y * size + x) * 4;
			switch (fallback)
			{
			case RPI_BEAM:			// a band across, solid along it
				dy = (y - 0.5f * size) / (size * 0.5f - 1);
				d = 256 * (1 - dy * dy);
				t[3] = (byte)(d < 0 ? 0 : d > 255 ? 255 : d);
				break;
			case RPI_FAN:			// light fading from a corner
				dy = y / (size * 0.5f - 1);
				dx = x / (size * 0.5f - 1);
				d = 256 * (1 - (dx + dy));
				d = d < 0 ? 0 : d > 255 ? 255 : d;
				t[0] = t[1] = t[2] = (byte)d;
				t[3] = (byte)(d / 2);
				break;
			case RPI_BALL:			// a round blob
				dy = (y - 0.5f * size) / (size * 0.5f - 1);
				dx = (x - 0.5f * size) / (size * 0.5f - 1);
				d = 256 * (1 - (dx * dx + dy * dy));
				t[3] = (byte)(d < 0 ? 0 : d > 255 ? 255 : d);
				break;
			case RPI_FUZZY:
				t[3] = (byte)(fuzzy[x][y] * 255 / 9);
				break;
			case RPI_CLASSIC:		// FitzQuake's round particle, in the top left quarter
				a = 8 * (255 - ((x - 16) * (x - 16) + (y - 16) * (y - 16) > 255 ? 255
					: (x - 16) * (x - 16) + (y - 16) * (y - 16)));
				t[3] = (byte)(a > 255 ? 255 : a);
				break;
			default:				// white
				break;
			}
		}
	return R_AddPartImage (r_partfallbacks[fallback], rgba, size, size);
}

/*
===============
R_ParticleImage

The image of a particle effect: textures/<name> or <name>, a TGA file, as
QuakeSpasm-Spiked looks for it; or, with no file of that name (or none
named), the fallback, made here, and *found false. Loaded once each.
===============
*/
int R_ParticleImage (const char *name, int fallback, bool *found)
{
	char	path[MAX_QPATH];
	byte	*rgba = NULL;
	int		i, w = 0, h = 0;
	const char	*prefix[] = {"textures/", ""};

	*found = false;
	if (name && *name)
	{
		for (i = 0 ; i < r_numpartimages ; i++)
			if (!strcmp (r_partimages[i].name, name))
			{
				*found = true;
				return i;
			}
		for (i = 0 ; i < 2 && !rgba ; i++)
		{
			snprintf (path, sizeof(path), "%s%s", prefix[i], name);
			if (!strstr (path, ".tga"))
				Q_strncatz (path, ".tga", sizeof(path));
			rgba = R_LoadTGA (path, &w, &h);
		}
		if (rgba && (i = R_AddPartImage (name, rgba, w, h)) >= 0)
		{
			*found = true;
			return i;
		}
	}

	if (fallback < 0 || fallback >= RPI_NUMFALLBACKS)
		fallback = RPI_FUZZY;
	for (i = 0 ; i < r_numpartimages ; i++)
		if (!strcmp (r_partimages[i].name, r_partfallbacks[fallback]))
			return i;
	return R_MakePartImage (fallback);
}

const partimage_t *R_PartImage (int index)
{
	return index >= 0 && index < r_numpartimages ? &r_partimages[index] : NULL;
}

// the images, all gone (a new game directory's are other files)
void R_FlushParticleImages (void)
{
	int		i, l;

	for (i = 0 ; i < r_numpartimages ; i++)
		for (l = 0 ; l < r_partimages[i].numlevels ; l++)
			Mem_Free (r_partimages[i].levels[l]);
	r_numpartimages = 0;
}
