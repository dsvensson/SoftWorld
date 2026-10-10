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
// d_sprite.c: software top-level rasterization driver module for drawing
// sprites

#include "r_local.h"
#include "d_local.h"

static int		sprite_height;
static sspan_t	*sprite_spans;


/*
=====================
D_SpriteDrawSpans
=====================
*/
static void D_SpriteDrawSpans (sspan_t *pspan)
{
	int			count, spancount;
	double		pixelzi;
	byte		*pbase;
	pixel_t		*pdest;
	fixed16_t	s, t, snext, tnext, sstep, tstep;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float		sdivz8stepu, tdivz8stepu, zi8stepu;
	byte		btemp;
	float		*pz;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	pbase = r_spritedesc.pspriteframe->pixels;

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

	do
	{
		pdest = d_viewbuffer + (screenwidth * pspan->v) + pspan->u;
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

		if (count <= 0)
			goto NextSpan;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
		pixelzi = zi;

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				btemp = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
				if (btemp != 255)
				{
					if (*pz <= pixelzi)
					{
						*pz = (float)pixelzi;
						*pdest = d_pal30_unlit[btemp];
					}
				}

				pixelzi += d_zistepu;
				pdest++;
				pz++;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

NextSpan:
		pspan++;

	} while (pspan->count != DS_SPAN_LIST_END);
}



/*
=====================
D_SpriteCalculateGradients
=====================
*/
static void D_SpriteCalculateGradients (void)
{
	vec3_t		p_normal, p_saxis, p_taxis, p_temp1;
	float		distinv;

	TransformVector (r_spritedesc.vpn, p_normal);
	TransformVector (r_spritedesc.vright, p_saxis);
	TransformVector (r_spritedesc.vup, p_taxis);
	VectorInverse (p_taxis);

	distinv = 1.0f / (-DotProduct (modelorg, r_spritedesc.vpn));

	d_sdivzstepu = p_saxis[0] * xscaleinv;
	d_tdivzstepu = p_taxis[0] * xscaleinv;

	d_sdivzstepv = -p_saxis[1] * yscaleinv;
	d_tdivzstepv = -p_taxis[1] * yscaleinv;

	d_zistepu = p_normal[0] * xscaleinv * distinv;
	d_zistepv = -p_normal[1] * yscaleinv * distinv;

	d_sdivzorigin = p_saxis[2] - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;
	d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu -
			ycenter * d_zistepv;

	TransformVector (modelorg, p_temp1);

	// half the sprite added (id subtracted it negated, a negative shifted left)
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) +
			((cachewidth >> 1) << 16);
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) +
			((sprite_height >> 1) << 16);

// -1 (-epsilon) so we never wander off the edge of the texture
	bbextents = (cachewidth << 16) - 1;
	bbextentt = (sprite_height << 16) - 1;
}


/*
=====================
D_SetSpriteSize
=====================
*/
void D_SetSpriteSize (int height)
{
	Mem_Free (sprite_spans);
	sprite_spans = Mem_Calloc ((size_t)height + 1, sizeof(*sprite_spans));
}

/*
=====================
D_DrawSprite
=====================
*/
void D_DrawSprite (void)
{
	cachewidth = r_spritedesc.pspriteframe->width;
	sprite_height = r_spritedesc.pspriteframe->height;

	D_SpriteCalculateGradients ();
	if (D_PolygonSpans (r_spritedesc.pverts, r_spritedesc.nump, sprite_spans, &r_refdef.vrect))
		D_SpriteDrawSpans (sprite_spans);
}

/*
=====================
D_FenceDrawSpans

Spans of a fence surface from its cache block: depth tested, depth writing,
the cut-out texels skipped
=====================
*/
static void D_FenceDrawSpans (sspan_t *pspan)
{
	int			count, spancount;
	float		pixelzi;
	pixel_t		*pdest, texel;
	fixed16_t	s, t, snext, tnext, sstep, tstep;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float		sdivz8stepu, tdivz8stepu, zi8stepu;
	float		*pz;

	sstep = 0;
	tstep = 0;

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

	for ( ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		count = pspan->count;
		if (count <= 0)
			continue;

		pdest = d_viewbuffer + (screenwidth * pspan->v) + pspan->u;
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

	// the initial s/z, t/z, 1/z, s and t, clamped
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;
		pixelzi = zi;

		s = (int)(sdivz * z) + sadjust;
		s = s > bbextents ? bbextents : (s < 0 ? 0 : s);
		t = (int)(tdivz * z) + tadjust;
		t = t > bbextentt ? bbextentt : (t < 0 ? 0 : t);

		do
		{
			spancount = count >= 8 ? 8 : count;
			count -= spancount;

			if (count)
			{
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				snext = snext > bbextents ? bbextents : (snext < 8 ? 8 : snext);
				tnext = (int)(tdivz * z) + tadjust;
				tnext = tnext > bbextentt ? bbextentt : (tnext < 8 ? 8 : tnext);
				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;
				snext = (int)(sdivz * z) + sadjust;
				snext = snext > bbextents ? bbextents : (snext < 8 ? 8 : snext);
				tnext = (int)(tdivz * z) + tadjust;
				tnext = tnext > bbextentt ? bbextentt : (tnext < 8 ? 8 : tnext);
				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				texel = cacheblock[(s >> 16) + (t >> 16) * cachewidth];
				if (!(texel & PIXEL_TRANSPARENT) && *pz <= pixelzi)
				{
					*pz = pixelzi;
					*pdest = texel;
				}
				pixelzi += d_zistepu;
				pdest++;
				pz++;
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;
		} while (count > 0);
	}
}

/*
=====================
D_DrawFencePolygon
=====================
*/
void D_DrawFencePolygon (emitpoint_t *pverts, int nump)
{
	if (D_PolygonSpans (pverts, nump, sprite_spans, &r_refdef.vrect))
		D_FenceDrawSpans (sprite_spans);
}

/*
=====================
D_DrawBlendedPolygon
=====================
*/
void D_DrawBlendedPolygon (emitpoint_t *pverts, int nump, int alpha, bool turb)
{
	if (D_PolygonSpans (pverts, nump, sprite_spans, &r_refdef.vrect))
		D_DrawBlendedSpans (sprite_spans, alpha, turb);
}

/*
=====================
D_DrawFlatPolygon

A projected polygon of one color over what is there, alpha of 256: depth
tested, not depth written. 1/z across it is the plane through its corners;
pverts has room for one more, and is turned clockwise if it isn't.
=====================
*/
void D_DrawFlatPolygon (emitpoint_t *pverts, int nump, pixel_t color, int alpha)
{
	sspan_t		*pspan;
	emitpoint_t	swap;
	pixel_t		*pdest;
	float		*pz;
	float		zi, zistepu, zistepv, ziorigin, area, det, best;
	float		du1, dv1, du2, dv2;
	int			i, j, count;

	// clockwise on the screen, as the edge scanning takes it
	area = 0;
	for (i=0 ; i<nump ; i++)
	{
		j = (i + 1) % nump;
		area += pverts[i].u * pverts[j].v - pverts[j].u * pverts[i].v;
	}
	if (area < 0)
		for (i=0, j=nump-1 ; i<j ; i++, j--)
		{
			swap = pverts[i];
			pverts[i] = pverts[j];
			pverts[j] = swap;
		}

	// 1/z through the first corner and the two that span most with it
	best = 0;
	zistepu = zistepv = 0;
	for (i=1 ; i<nump ; i++)
		for (j=i+1 ; j<nump ; j++)
		{
			du1 = pverts[i].u - pverts[0].u;
			dv1 = pverts[i].v - pverts[0].v;
			du2 = pverts[j].u - pverts[0].u;
			dv2 = pverts[j].v - pverts[0].v;
			det = du1 * dv2 - du2 * dv1;
			if (fabsf (det) > best)
			{
				best = fabsf (det);
				zistepu = ((pverts[i].zi - pverts[0].zi) * dv2 - (pverts[j].zi - pverts[0].zi) * dv1) / det;
				zistepv = (du1 * (pverts[j].zi - pverts[0].zi) - du2 * (pverts[i].zi - pverts[0].zi)) / det;
			}
		}
	if (best < 0.01f)
		return;		// seen edge on
	ziorigin = pverts[0].zi - zistepu * pverts[0].u - zistepv * pverts[0].v;

	if (!D_PolygonSpans (pverts, nump, sprite_spans, &r_refdef.vrect))
		return;
	for (pspan = sprite_spans ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		count = pspan->count;
		if (count <= 0)
			continue;
		pdest = d_viewbuffer + (screenwidth * pspan->v) + pspan->u;
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;
		zi = ziorigin + pspan->v * zistepv + pspan->u * zistepu;
		for ( ; count ; count--, pdest++, pz++, zi += zistepu)
			if (*pz <= zi)
				*pdest = D_BlendPixel (color, *pdest, alpha);
	}
}

