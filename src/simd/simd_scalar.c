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

#include <math.h>
#include <string.h>

void Simd_Scalar_ZSpan (float *dest, int count, float zi, float step)
{
	int		i;

	for (i = 0 ; i < count ; i++)
		dest[i] = zi + (float)i * step;
}

// the perspective stepping of a span, as the original span drawers do it: s
// and t exact at the ends of each subdivision of 1 << shift pixels, stepped
// in between
typedef struct
{
	const simd_texmap_t	*map;
	float	sdivz, tdivz, zi;
	int		s, t;			// where the next subdivision starts
	int		sstep, tstep;
	int		count;			// pixels left
	int		shift;
} scalar_stepper_t;

// hi if above it, else lo if below it
static int Simd_Scalar_Clamp (int x, int lo, int hi)
{
	if (x > hi)
		return hi;
	if (x < lo)
		return lo;
	return x;
}

static inline void Simd_Scalar_SpanStart (scalar_stepper_t *st, const simd_texmap_t *map, int u, int v,
	int count, int shift)
{
	float	du = (float)u, dv = (float)v, z;

	st->map = map;
	st->count = count;
	st->shift = shift;
	st->sstep = 0;
	st->tstep = 0;
	st->sdivz = map->sdivzorigin + dv*map->sdivzstepv + du*map->sdivzstepu;
	st->tdivz = map->tdivzorigin + dv*map->tdivzstepv + du*map->tdivzstepu;
	st->zi = map->ziorigin + dv*map->zistepv + du*map->zistepu;
	z = (float)0x10000 / st->zi;	// prescale to 16.16 fixed-point
	st->s = Simd_Scalar_Clamp ((int)(st->sdivz * z) + map->sadjust, 0, map->sextent);
	st->t = Simd_Scalar_Clamp ((int)(st->tdivz * z) + map->tadjust, 0, map->textent);
}

// the next subdivision: its first s and t, with st->sstep and st->tstep for
// it; returns its pixel count. Inline: clang otherwise calls it, every 8
// pixels, where GCC unrolls the pixels around it.
static inline int Simd_Scalar_SpanNext (scalar_stepper_t *st, int *s, int *t)
{
	const simd_texmap_t	*map = st->map;
	int		n = 1 << st->shift, spancount, snext, tnext;
	float	z, spancountminus1;

	spancount = st->count >= n ? n : st->count;
	st->count -= spancount;
	if (st->count)
	{
	// s and t at the start of the next subdivision, the steps by shifting.
	// The least s and t is n, so rounding down negative steps can't run off
	// the texture.
		st->sdivz += map->sdivzstepu * (float)n;
		st->tdivz += map->tdivzstepu * (float)n;
		st->zi += map->zistepu * (float)n;
		z = (float)0x10000 / st->zi;
		snext = Simd_Scalar_Clamp ((int)(st->sdivz * z) + map->sadjust, n, map->sextent);
		tnext = Simd_Scalar_Clamp ((int)(st->tdivz * z) + map->tadjust, n, map->textent);
		st->sstep = (snext - st->s) >> st->shift;
		st->tstep = (tnext - st->t) >> st->shift;
	}
	else
	{
	// s and t at the span's last pixel, so it can't step off the polygon; the
	// steps by division, rounding toward the start
		spancountminus1 = (float)(spancount - 1);
		st->sdivz += map->sdivzstepu * spancountminus1;
		st->tdivz += map->tdivzstepu * spancountminus1;
		st->zi += map->zistepu * spancountminus1;
		z = (float)0x10000 / st->zi;
		snext = Simd_Scalar_Clamp ((int)(st->sdivz * z) + map->sadjust, n, map->sextent);
		tnext = Simd_Scalar_Clamp ((int)(st->tdivz * z) + map->tadjust, n, map->textent);
		if (spancount > 1)
		{
			st->sstep = (snext - st->s) / (spancount - 1);
			st->tstep = (tnext - st->t) / (spancount - 1);
		}
	}
	*s = st->s;
	*t = st->t;
	st->s = snext;
	st->t = tnext;
	return spancount;
}

void Simd_Scalar_TexSpan (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src, int srcwidth,
	int u, int v, int count)
{
	scalar_stepper_t	st;
	int					n, s0, t0, s, t, sstep, tstep;

	Simd_Scalar_SpanStart (&st, map, u, v, count, 3);
	do
	{
		// stepped in copies no pointer has seen, which stay in registers; the
		// stores through dest might otherwise be to s0 and t0
		n = Simd_Scalar_SpanNext (&st, &s0, &t0);
		s = s0;
		t = t0;
		sstep = st.sstep;
		tstep = st.tstep;
		for ( ; n > 0 ; n--, s += sstep, t += tstep)
			*dest++ = src[(s >> 16) + (t >> 16) * srcwidth];
	} while (st.count > 0);
}

void Simd_Scalar_TurbSpan (uint32_t *dest, const simd_texmap_t *map, const byte *src, const uint32_t *palette,
	const int *turb, int u, int v, int count)
{
	scalar_stepper_t	st;
	int					n, s0, t0, s, t, sstep, tstep, sturb, tturb;

	Simd_Scalar_SpanStart (&st, map, u, v, count, 4);
	do
	{
		// stepped in copies, as in Simd_Scalar_TexSpan
		n = Simd_Scalar_SpanNext (&st, &s0, &t0);
		s = s0 & ((128 << 16) - 1);
		t = t0 & ((128 << 16) - 1);
		sstep = st.sstep;
		tstep = st.tstep;
		for ( ; n > 0 ; n--, s += sstep, t += tstep)
		{
			sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
			tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
			*dest++ = palette[src[(tturb << 6) + sturb]];
		}
	} while (st.count > 0);
}

void Simd_Scalar_TurbSpanRGB30 (uint32_t *dest, const simd_texmap_t *map, const uint32_t *src,
	const int *turb, int u, int v, int count)
{
	scalar_stepper_t	st;
	int					n, s0, t0, s, t, sstep, tstep, sturb, tturb;

	Simd_Scalar_SpanStart (&st, map, u, v, count, 4);
	do
	{
		n = Simd_Scalar_SpanNext (&st, &s0, &t0);
		s = s0 & ((128 << 16) - 1);
		t = t0 & ((128 << 16) - 1);
		sstep = st.sstep;
		tstep = st.tstep;
		for ( ; n > 0 ; n--, s += sstep, t += tstep)
		{
			sturb = ((s + turb[(t >> 16) & 127]) >> 16) & 63;
			tturb = ((t + turb[(s >> 16) & 127]) >> 16) & 63;
			*dest++ = src[(tturb << 6) + sturb];
		}
	} while (st.count > 0);
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

void Simd_Scalar_LitRowRGB30 (uint32_t *dest, const uint32_t *src, const uint32_t *glow, int glowscale,
	const int light[3], const int step[3], int count)
{
	unsigned	c, f;
	uint32_t	p, g, out;
	int			j, k, l;

	for (j = 0 ; j < count ; j++)
	{
		p = src[j];
		g = glow ? glow[j] : 0;
		out = 0;
		for (k = 0 ; k < 3 ; k++)
		{
			l = light[k] + (count - 1 - j) * step[k];
			c = (((p >> (10 * k)) & 1023) * (unsigned)(l > 0 ? l : 0)) >> 15;
			f = (((g >> (10 * k)) & 1023) * (unsigned)glowscale) >> 15;
			if (c < f)
				c = f;
			if (c > 1023)
				c = 1023;
			out |= c << (10 * k);
		}
		dest[j] = out;
	}
}

// an alias model pixel lit in RGB: the light's level times the tint, then as
// simd_litrow_rgb
static uint32_t Simd_Scalar_AliasLit (const simd_aliasmap_t *map, uint32_t index, int light)
{
	unsigned	level = light < (255 << 6) ? (unsigned)((255 << 6) - light) : 0;	// 8192 is 1.0
	uint32_t	p = map->palette[index], fl = map->floor[index], out = 0;
	unsigned	l, c, f;
	int			k;

	for (k = 0 ; k < 3 ; k++)
	{
		l = (level * map->tint[k]) >> 6;
		c = (((p >> (10 * k)) & 1023) * l) >> 15;
		f = (fl >> (10 * k)) & 1023;
		if (c < f)
			c = f;
		if (c > 1023)
			c = 1023;
		out |= c << (10 * k);
	}
	return out;
}

void Simd_Scalar_AliasSpan (uint32_t *dest, float *zbuf, const byte *tex, int sfrac, int tfrac,
	int light, int zi, int count, const simd_aliasmap_t *map)
{
	uint32_t	index;
	float		z;
	int			i;

	for (i = 0 ; i < count ; i++)
	{
		z = (float)zi * (1.0f / 2147483648.0f);
		if (z >= zbuf[i])
		{
			index = map->remap[*tex];
			dest[i] = map->colormap ? map->colormap[index + (light & 0xFF00)] : Simd_Scalar_AliasLit (map, index, light);
			zbuf[i] = z;
		}
		zi = (int)((unsigned)zi + (unsigned)map->zistep);
		light = (int)((unsigned)light + (unsigned)map->lightstep);
		tex += map->stepwhole;
		sfrac += map->sfracstep;
		tex += sfrac >> 16;
		sfrac &= 0xFFFF;
		tfrac += map->tfracstep;
		if (tfrac & 0x10000)
		{
			tex += map->skinwidth;
			tfrac &= 0xFFFF;
		}
	}
}

// a channel of src over dest in linear light, a channel being the fourth root
// of its light: ((s^4 * a + d^4 * ia) / 256)^(1/4)
static inline unsigned Simd_Scalar_BlendChannel (unsigned s, unsigned d, float a, float ia)
{
	float		fs = (float)s, fd = (float)d;
	unsigned	c;

	fs *= fs;
	fs *= fs;
	fd *= fd;
	fd *= fd;
	c = (unsigned)(sqrtf (sqrtf ((fs * a + fd * ia) * (1.0f / 256.0f))) + 0.5f);
	return c < 1023 ? c : 1023;
}

void Simd_Scalar_BlendSpan (uint32_t *dest, const uint32_t *src, const float *zbuf, float zi, float step,
	int alpha, int count)
{
	float		a = (float)alpha, ia = (float)(256 - alpha);
	uint32_t	s, d;
	int			i;

	for (i = 0 ; i < count ; i++)
	{
		s = src[i];
		if (s & 0x80000000u)
			continue;
		if (zbuf && !(zbuf[i] <= zi + (float)i * step))
			continue;
		d = dest[i];
		dest[i] = Simd_Scalar_BlendChannel (s & 1023, d & 1023, a, ia)
			| (Simd_Scalar_BlendChannel ((s >> 10) & 1023, (d >> 10) & 1023, a, ia) << 10)
			| (Simd_Scalar_BlendChannel ((s >> 20) & 1023, (d >> 20) & 1023, a, ia) << 20);
	}
}

// a channel's light d^4 times m, plus s, back to its fourth root
static inline unsigned Simd_Scalar_PartChannel (unsigned d, float s, float m)
{
	float		fd = (float)d, x;
	unsigned	c;

	fd *= fd;
	fd *= fd;
	x = s + fd * m;
	x = x > 0 ? x : 0;
	c = (unsigned)(sqrtf (sqrtf (x)) + 0.5f);
	return c < 1023 ? c : 1023;
}

void Simd_Scalar_PartSpan (uint32_t *dest, const float *zbuf, float zi, float step,
	const float *const src[3], const float *const mul[3], int count)
{
	uint32_t	d;
	int			i;

	for (i = 0 ; i < count ; i++)
	{
		if (zbuf && !(zbuf[i] <= zi + (float)i * step))
			continue;
		d = dest[i];
		dest[i] = Simd_Scalar_PartChannel (d & 1023, src[0][i], mul[0][i])
			| (Simd_Scalar_PartChannel ((d >> 10) & 1023, src[1][i], mul[1][i]) << 10)
			| (Simd_Scalar_PartChannel ((d >> 20) & 1023, src[2][i], mul[2][i]) << 20);
	}
}

// a channel fogged: its light weighted a (of 256) and the fog's, fog, ia, as
// Simd_Scalar_BlendChannel blends
static inline unsigned Simd_Scalar_FogChannel (unsigned s, float fog, float a, float ia)
{
	float		fs = (float)s;
	unsigned	c;

	fs *= fs;
	fs *= fs;
	c = (unsigned)(sqrtf (sqrtf ((fs * a + fog * ia) * (1.0f / 256.0f))) + 0.5f);
	return c < 1023 ? c : 1023;
}

// the fog table's entry for a 1/z, by its bits
static inline int Simd_Scalar_FogEntry (float zi, const simd_fog_t *fog)
{
	int32_t		bits;
	int			e;

	memcpy (&bits, &zi, sizeof(bits));
	if (bits < 0)
		return 0;
	e = (bits >> SIMD_FOG_SHIFT) - fog->base;
	return e < 0 ? 0 : e < fog->size ? e : fog->size - 1;
}

void Simd_Scalar_FogSpan (uint32_t *dest, const float *zbuf, float zi, float step, int count,
	const simd_fog_t *fog)
{
	float		a, ia;
	uint32_t	p;
	int			i;

	for (i = 0 ; i < count ; i++)
	{
		p = dest[i];
		if (p & 0x80000000u)
			continue;
		if (p & 0x40000000u)
			a = fog->sky;
		else
			a = fog->table[Simd_Scalar_FogEntry (zbuf ? zbuf[i] : zi + (float)i * step, fog)];
		ia = 256.0f - a;
		dest[i] = Simd_Scalar_FogChannel (p & 1023, fog->color[0], a, ia)
			| (Simd_Scalar_FogChannel ((p >> 10) & 1023, fog->color[1], a, ia) << 10)
			| (Simd_Scalar_FogChannel ((p >> 20) & 1023, fog->color[2], a, ia) << 20);
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
