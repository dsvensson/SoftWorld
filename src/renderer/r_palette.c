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
// r_palette.c -- the palette and the colormap as RGB30 pixels

#include "r_local.h"

pixel_t	d_pal30[256];
pixel_t	d_cm30[VID_GRADES * 256];
byte	r_identityremap[256];
byte	d_palrgb[256][3];		// the palette, for lighting by multiplication
static bool	d_fullbright[256];	// colors light doesn't change
pixel_t	d_pal30_floor[256];		// the least a lit color can be: fullbrights brightened
								// by r_fullbright_scale, 0 for the others

/*
===============
R_InitPalette

The colormap holds, for each light level, the palette index every color
turns into; d_cm30 holds those colors themselves.
===============
*/
void R_InitPalette (const byte *palette, const byte *colormap)
{
	int		i;

	for (i = 0 ; i < 256 ; i++)
	{
		d_pal30[i] = RGB30 (palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2]);
		r_identityremap[i] = (byte)i;
	}
	for (i = 0 ; i < VID_GRADES * 256 ; i++)
		d_cm30[i] = d_pal30[colormap[i]];

	for (i = 0 ; i < 256 ; i++)
	{
		d_palrgb[i][0] = palette[i * 3];
		d_palrgb[i][1] = palette[i * 3 + 1];
		d_palrgb[i][2] = palette[i * 3 + 2];
		// the same color in the brightest and the darkest row
		d_fullbright[i] = colormap[i] == i && colormap[(VID_GRADES - 1) * 256 + i] == i && i;
	}
	R_SetFullbrightScale (1);
}

/*
===============
R_SetFullbrightScale
===============
*/
void R_SetFullbrightScale (float scale)
{
	int		i, c;
	unsigned	v[3];

	for (i = 0 ; i < 256 ; i++)
	{
		for (c = 0 ; c < 3 ; c++)
		{
			v[c] = (unsigned)(d_palrgb[i][c] * scale + 0.5f);
			if (v[c] > 1023)
				v[c] = 1023;
		}
		d_pal30_floor[i] = d_fullbright[i] ? RGB30 (v[0], v[1], v[2]) : 0;
	}
}

/*
===============
R_NearestColor

The palette color nearest to r, g, b among the fullbright colors or the
others, optionally leaving out 255 (a fence's cut out). Cached at 6 bits a
channel; the palette is set once.
===============
*/
static unsigned short	*r_nearest;		// [4][64*64*64], 0xffff until found

static byte R_NearestColor (int r, int g, int b, bool fullbright, bool no255)
{
	int		set = (fullbright ? 1 : 0) | (no255 ? 2 : 0);
	int		key = ((set * 64 + (r >> 2)) * 64 + (g >> 2)) * 64 + (b >> 2);
	int		i, d, dr, dg, db, best, bestdist;

	if (!r_nearest)
	{
		r_nearest = Mem_Alloc (4 * 64 * 64 * 64 * sizeof(*r_nearest));
		memset (r_nearest, 0xff, 4 * 64 * 64 * 64 * sizeof(*r_nearest));
	}
	if (r_nearest[key] != 0xffff)
		return (byte)r_nearest[key];

	best = -1;
	bestdist = 0x7fffffff;
	for (i = 0 ; i < 256 ; i++)
	{
		if (d_fullbright[i] != fullbright || (no255 && i == 255))
			continue;
		dr = d_palrgb[i][0] - ((r & ~3) + 2);
		dg = d_palrgb[i][1] - ((g & ~3) + 2);
		db = d_palrgb[i][2] - ((b & ~3) + 2);
		d = dr*dr + dg*dg + db*db;
		if (d < bestdist)
		{
			bestdist = d;
			best = i;
		}
	}
	if (best < 0)
		best = 0;	// no color of that kind
	r_nearest[key] = (unsigned short)best;
	return (byte)best;
}

/*
===============
R_BuildMips

Mip levels 1 to 3 of a texture from the level above: each texel the average
of four, as the nearest palette color. Mostly fullbright blocks stay
fullbright; in a fence, a block half cut out or more stays cut out.
===============
*/
void R_BuildMips (texture_t *tx, bool fence)
{
	int			mip, x, y, i, w, h, n, nbright, c, sum[3];
	const byte	*src;
	byte		*dest, block[4];
	bool		bright;

	for (mip = 1 ; mip < MIPLEVELS ; mip++)
	{
		src = (byte *)tx + tx->offsets[mip - 1];
		dest = (byte *)tx + tx->offsets[mip];
		w = (int)(tx->width >> mip);
		h = (int)(tx->height >> mip);
		for (y = 0 ; y < h ; y++)
		{
			for (x = 0 ; x < w ; x++)
			{
				block[0] = src[(y*2) * w*2 + x*2];
				block[1] = src[(y*2) * w*2 + x*2 + 1];
				block[2] = src[(y*2 + 1) * w*2 + x*2];
				block[3] = src[(y*2 + 1) * w*2 + x*2 + 1];

				n = nbright = 0;
				for (i = 0 ; i < 4 ; i++)
				{
					if (fence && block[i] == 255)
						continue;
					n++;
					if (d_fullbright[block[i]])
						nbright++;
				}
				if (fence && n <= 2)
				{
					dest[y * w + x] = 255;
					continue;
				}

				bright = nbright * 2 > n;
				sum[0] = sum[1] = sum[2] = 0;
				n = 0;
				for (i = 0 ; i < 4 ; i++)
				{
					if ((fence && block[i] == 255) || d_fullbright[block[i]] != bright)
						continue;
					for (c = 0 ; c < 3 ; c++)
						sum[c] += d_palrgb[block[i]][c];
					n++;
				}
				dest[y * w + x] = R_NearestColor (sum[0] / n, sum[1] / n, sum[2] / n, bright, fence);
			}
		}
	}
}
