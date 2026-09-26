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
// simd_scalar.c -- the reference kernels, plain C; every other backend
// matches these bit for bit

#include "simd_backends.h"

#include <string.h>

void Simd_Scalar_ZSpan (float *dest, int count, float zi, float step)
{
	int		i;

	for (i = 0 ; i < count ; i++)
		dest[i] = zi + (float)i * step;
}

void Simd_Scalar_TexSpan (uint32_t *dest, const uint32_t *src, int srcwidth,
	int s, int t, int sstep, int tstep, int count)
{
	int		i;

	for (i = 0 ; i < count ; i++)
		dest[i] = src[((s + i * sstep) >> 16) + ((t + i * tstep) >> 16) * srcwidth];
}

void Simd_Scalar_LitRowColormap (uint32_t *dest, const byte *src, const uint32_t *colormap,
	int light, int step, int count)
{
	int		j;

	for (j = 0 ; j < count ; j++)
		dest[j] = colormap[((light + (count - 1 - j) * step) & 0xFF00) + src[j]];
}

void Simd_Scalar_LitRowRGB (uint32_t *dest, const byte *src, const uint32_t *palette,
	const uint32_t *floor, const int light[3], const int step[3], int count)
{
	unsigned	c[3], f[3];
	uint32_t	p, fl;
	int			j, k, l;

	for (j = 0 ; j < count ; j++)
	{
		p = palette[src[j]];
		fl = floor[src[j]];
		for (k = 0 ; k < 3 ; k++)
		{
			l = light[k] + (count - 1 - j) * step[k];
			c[k] = (((p >> (10 * k)) & 1023) * (unsigned)(l > 0 ? l : 0)) >> 15;
			f[k] = (fl >> (10 * k)) & 1023;
			if (c[k] < f[k])
				c[k] = f[k];
			if (c[k] > 1023)
				c[k] = 1023;
		}
		dest[j] = c[0] | (c[1] << 10) | (c[2] << 20);
	}
}

void Simd_Scalar_Expand8 (uint32_t *dest, const byte *src, const uint32_t *palette, int count,
	int scale, int transparent)
{
	uint32_t	p;
	int			i, k;

	for (i = 0 ; i < count ; i++, dest += scale)
	{
		if (src[i] == transparent)
			continue;
		p = palette[src[i]];
		for (k = 0 ; k < scale ; k++)
			dest[k] = p;
	}
}

void Simd_Scalar_CopyStream (void *dest, size_t destpitch, const void *src, size_t srcpitch,
	size_t rowbytes, int rows)
{
	byte		*d = dest;
	const byte	*s = src;
	int			y;

	for (y = 0 ; y < rows ; y++, d += destpitch, s += srcpitch)
		memcpy (d, s, rowbytes);
}
