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
static int		minindex, maxindex;
static sspan_t	*sprite_spans;


/*
=====================
D_SpriteDrawSpans
=====================
*/
void D_SpriteDrawSpans (sspan_t *pspan)
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
						*pdest = d_pal30[btemp];
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
D_ScanLeftEdge
=====================
*/
static void D_ScanLeftEdge (emitpoint_t *pverts, int nump, sspan_t *spans)
{
	int			i, v, itop, ibottom, lmaxindex;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope;
	fixed16_t	u, u_step;

	pspan = spans;
	i = minindex;
	if (i == 0)
		i = nump;

	lmaxindex = maxindex;
	if (lmaxindex == 0)
		lmaxindex = nump;

	vtop = ceilf(pverts[i].v);

	do
	{
		pvert = &pverts[i];
		pnext = pvert - 1;

		vbottom = ceilf(pnext->v);

		if (vtop < vbottom)
		{
			du = pnext->u - pvert->u;
			dv = pnext->v - pvert->v;
			slope = du / dv;
			u_step = (int)(slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
					(0x10000 - 1);
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->u = u >> 16;
				pspan->v = v;
				u += u_step;
				pspan++;
			}
		}

		vtop = vbottom;

		i--;
		if (i == 0)
			i = nump;

	} while (i != lmaxindex);
}


/*
=====================
D_ScanRightEdge
=====================
*/
static void D_ScanRightEdge (emitpoint_t *pverts, int nump, sspan_t *spans)
{
	int			i, v, itop, ibottom;
	emitpoint_t	*pvert, *pnext;
	sspan_t		*pspan;
	float		du, dv, vtop, vbottom, slope, uvert, unext, vvert, vnext;
	fixed16_t	u, u_step;

	pspan = spans;
	i = minindex;

	vvert = pverts[i].v;
	if (vvert < r_refdef.fvrecty_adj)
		vvert = r_refdef.fvrecty_adj;
	if (vvert > r_refdef.fvrectbottom_adj)
		vvert = r_refdef.fvrectbottom_adj;

	vtop = ceilf(vvert);

	do
	{
		pvert = &pverts[i];
		pnext = pvert + 1;

		vnext = pnext->v;
		if (vnext < r_refdef.fvrecty_adj)
			vnext = r_refdef.fvrecty_adj;
		if (vnext > r_refdef.fvrectbottom_adj)
			vnext = r_refdef.fvrectbottom_adj;

		vbottom = ceilf(vnext);

		if (vtop < vbottom)
		{
			uvert = pvert->u;
			if (uvert < r_refdef.fvrectx_adj)
				uvert = r_refdef.fvrectx_adj;
			if (uvert > r_refdef.fvrectright_adj)
				uvert = r_refdef.fvrectright_adj;

			unext = pnext->u;
			if (unext < r_refdef.fvrectx_adj)
				unext = r_refdef.fvrectx_adj;
			if (unext > r_refdef.fvrectright_adj)
				unext = r_refdef.fvrectright_adj;

			du = unext - uvert;
			dv = vnext - vvert;
			slope = du / dv;
			u_step = (int)(slope * 0x10000);
		// adjust u to ceil the integer portion
			u = (int)((uvert + (slope * (vtop - vvert))) * 0x10000) +
					(0x10000 - 1);
			itop = (int)vtop;
			ibottom = (int)vbottom;

			for (v=itop ; v<ibottom ; v++)
			{
				pspan->count = (u >> 16) - pspan->u;
				u += u_step;
				pspan++;
			}
		}

		vtop = vbottom;
		vvert = vnext;

		i++;
		if (i == nump)
			i = 0;

	} while (i != maxindex);

	pspan->count = DS_SPAN_LIST_END;	// mark the end of the span list 
}


/*
=====================
D_SpriteCalculateGradients
=====================
*/
void D_SpriteCalculateGradients (void)
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

	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) -
			(-(cachewidth >> 1) << 16);
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) -
			(-(sprite_height >> 1) << 16);

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
D_PolygonSpans
=====================
*/
bool D_PolygonSpans (emitpoint_t *pverts, int nump, sspan_t *spans)
{
	int			i;
	float		ymin, ymax;

// find the top and bottom vertices, and make sure there's at least one scan to
// draw
	ymin = 999999.9f;
	ymax = -999999.9f;
	for (i=0 ; i<nump ; i++)
	{
		if (pverts[i].v < ymin)
		{
			ymin = pverts[i].v;
			minindex = i;
		}
		if (pverts[i].v > ymax)
		{
			ymax = pverts[i].v;
			maxindex = i;
		}
	}

	ymin = ceilf(ymin);
	ymax = ceilf(ymax);
	if (ymin >= ymax)
		return false;		// doesn't cross any scans at all

// copy the first vertex to the last vertex, so we don't have to deal with
// wrapping
	pverts[nump] = pverts[0];

	D_ScanLeftEdge (pverts, nump, spans);
	D_ScanRightEdge (pverts, nump, spans);
	return true;
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
	if (D_PolygonSpans (r_spritedesc.pverts, r_spritedesc.nump, sprite_spans))
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
	if (D_PolygonSpans (pverts, nump, sprite_spans))
		D_FenceDrawSpans (sprite_spans);
}

/*
=====================
D_DrawBlendedPolygon
=====================
*/
void D_DrawBlendedPolygon (emitpoint_t *pverts, int nump, int alpha, bool turb)
{
	if (D_PolygonSpans (pverts, nump, sprite_spans))
		D_DrawBlendedSpans (sprite_spans, alpha, turb);
}

