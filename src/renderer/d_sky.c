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
// d_sky.c

#include "r_local.h"
#include "r_local.h"
#include "d_local.h"

#define SKY_SPAN_SHIFT	5
#define SKY_SPAN_MAX	(1 << SKY_SPAN_SHIFT)


/*
=================
D_Sky_uv_To_st
=================
*/
static void D_Sky_uv_To_st (int u, int v, fixed16_t *s, fixed16_t *t)
{
	float	wu, wv, temp;
	vec3_t	end;

	if (r_refdef.vrect.width >= r_refdef.vrect.height)
		temp = (float)r_refdef.vrect.width;
	else
		temp = (float)r_refdef.vrect.height;

	wu = 8192.0f * (float)(u-((int)vid.width>>1)) / temp;
	wv = 8192.0f * (float)(((int)vid.height>>1)-v) / temp;

	end[0] = 4096*vpn[0] + wu*vright[0] + wv*vup[0];
	end[1] = 4096*vpn[1] + wu*vright[1] + wv*vup[1];
	end[2] = 4096*vpn[2] + wu*vright[2] + wv*vup[2];
	end[2] *= 3;
	VectorNormalize (end);

	temp = skytime*skyspeed;	// TODO: add D_SetupFrame & set this there
	*s = (int)((temp + 6*(SKYSIZE/2-1)*end[0]) * 0x10000);
	*t = (int)((temp + 6*(SKYSIZE/2-1)*end[1]) * 0x10000);
}


/*
=================
D_DrawSkyScans
=================
*/
void D_DrawSkyScans (espan_t *pspan)
{
	int				count, spancount, u, v;
	pixel_t			*pdest;
	pixel_t			sky = r_fogactive ? PIXEL_SKY : 0;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	int				spancountminus1;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto
	snext = 0;	// ditto
	tnext = 0;	// ditto

	do
	{
		pdest = d_viewbuffer + (screenwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial s & t
		u = pspan->u;
		v = pspan->v;
		D_Sky_uv_To_st (u, v, &s, &t);

		do
		{
			if (count >= SKY_SPAN_MAX)
				spancount = SKY_SPAN_MAX;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				u += spancount;

			// calculate s and t at far end of span,
			// calculate s and t steps across span by shifting
				D_Sky_uv_To_st (u, v, &snext, &tnext);

				sstep = (snext - s) >> SKY_SPAN_SHIFT;
				tstep = (tnext - t) >> SKY_SPAN_SHIFT;
			}
			else
			{
			// calculate s and t at last pixel in span,
			// calculate s and t steps across span by division
				spancountminus1 = (int)((float)(spancount - 1));

				if (spancountminus1 > 0)
				{
					u += spancountminus1;
					D_Sky_uv_To_st (u, v, &snext, &tnext);

					sstep = (snext - s) / spancountminus1;
					tstep = (tnext - t) / spancountminus1;
				}
			}

			do
			{
				*pdest++ = d_pal30_unlit[r_skysource[((t & R_SKY_TMASK) >> 8) +
						((s & R_SKY_SMASK) >> 16)]] | sky;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}



/*
===============================================================================

SKYBOX

===============================================================================
*/

#define SKYBOX_SPAN		16		// pixels between exact texel coordinates

/*
=================
D_SkyboxTexel

Where direction d meets the skybox: the face it returns (rt bk lf ft up dn),
and s and t across it from the face's top left, 16.16 texels
=================
*/
static int D_SkyboxTexel (const float d[3], int *s, int *t)
{
	float	ax = fabsf (d[0]), ay = fabsf (d[1]), az = fabsf (d[2]);
	float	sc, tc, m, half = (float)r_skyboxsize * 32768.0f;
	int		face, si, ti, max = r_skyboxsize * 0x10000 - 1;

	if (ax >= ay && ax >= az)
	{
		m = ax;
		face = d[0] > 0 ? 0 : 2;
		sc = d[0] > 0 ? -d[1] : d[1];
		tc = -d[2];
	}
	else if (ay >= az)
	{
		m = ay;
		face = d[1] > 0 ? 1 : 3;
		sc = d[1] > 0 ? d[0] : -d[0];
		tc = -d[2];
	}
	else
	{
		m = az;
		face = d[2] > 0 ? 4 : 5;
		sc = -d[1];
		tc = d[2] > 0 ? d[0] : -d[0];
	}
	// -1 .. 1 across the face is 0 .. size
	m = half / m;
	si = (int)(sc * m + half);
	ti = (int)(tc * m + half);
	*s = si < 0 ? 0 : si > max ? max : si;
	*t = ti < 0 ? 0 : ti > max ? max : ti;
	return face;
}

/*
=================
D_DrawSkyboxScans

Spans of the sky as the skybox is seen through them. A face's part of the
screen is convex, so where the pixels SKYBOX_SPAN apart both see one face,
all those between do, and its texel coordinates are stepped between theirs.
=================
*/
void D_DrawSkyboxScans (espan_t *pspan)
{
	float			origin[3], du[3], dv[3], d[3];
	int				i, c, u, n, count, size = r_skyboxsize;
	int				face, s, t, facenext, snext, tnext, sstep, tstep;
	pixel_t			*pdest;
	pixel_t			sky = r_fogactive ? PIXEL_SKY : 0;
	const pixel_t	*texels;

	// the direction pixel (u, v) looks in is origin + u*du + v*dv
	for (c=0 ; c<3 ; c++)
	{
		du[c] = xscaleinv * vright[c];
		dv[c] = -yscaleinv * vup[c];
		origin[c] = vpn[c] - xcenter * du[c] - ycenter * dv[c];
	}

	do
	{
		pdest = d_viewbuffer + (screenwidth * pspan->v) + pspan->u;
		u = pspan->u;
		count = pspan->count;
		for (c=0 ; c<3 ; c++)
			d[c] = origin[c] + (float)u * du[c] + (float)pspan->v * dv[c];
		face = D_SkyboxTexel (d, &s, &t);

		while (count > 0)
		{
			n = count < SKYBOX_SPAN ? count : SKYBOX_SPAN;
			for (c=0 ; c<3 ; c++)
				d[c] = origin[c] + (float)(u + n) * du[c] + (float)pspan->v * dv[c];
			facenext = D_SkyboxTexel (d, &snext, &tnext);

			if (facenext == face)
			{
				texels = r_skyfaces + (size_t)face * size * size;
				sstep = (snext - s) / n;
				tstep = (tnext - t) / n;
				for (i=0 ; i<n ; i++, s += sstep, t += tstep)
					*pdest++ = texels[(t >> 16) * size + (s >> 16)] | sky;
			}
			else
			{
				// a seam: each pixel exactly
				for (i=0 ; i<n ; i++)
				{
					for (c=0 ; c<3 ; c++)
						d[c] = origin[c] + (float)(u + i) * du[c] + (float)pspan->v * dv[c];
					face = D_SkyboxTexel (d, &s, &t);
					*pdest++ = r_skyfaces[((size_t)face * size + (size_t)(t >> 16)) * size + (size_t)(s >> 16)] | sky;
				}
			}

			u += n;
			count -= n;
			face = facenext;
			s = snext;
			t = tnext;
		}
	} while ((pspan = pspan->pnext) != NULL);
}
