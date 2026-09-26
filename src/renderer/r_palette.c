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
bool	d_fullbright[256];		// colors light doesn't change
pixel_t	d_pal30_fb[256];		// fullbright colors, brightened by r_fullbright_scale

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
		d_pal30_fb[i] = RGB30 (v[0], v[1], v[2]);
	}
}
