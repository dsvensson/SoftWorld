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
D_SpriteSpan

A span of the sprite map maps, on line v from u: its texels depth tested,
and depth written where drawn
=====================
*/
void D_SpriteSpan (const d_spritemap_t *map, int u, int v, int count)
{
	int			spancount;
	double		pixelzi;
	const byte	*pbase = map->pixels;
	pixel_t		*pdest;
	fixed16_t	s, t, snext, tnext, sstep, tstep;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float		sdivz8stepu, tdivz8stepu, zi8stepu;
	byte		btemp;
	float		*pz;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	if (count <= 0)
		return;

	sdivz8stepu = map->sdivzstepu * 8;
	tdivz8stepu = map->tdivzstepu * 8;
	zi8stepu = map->zistepu * 8;

	pdest = d_viewbuffer + (screenwidth * v) + u;
	pz = d_pzbuffer + (d_zwidth * v) + u;

// calculate the initial s/z, t/z, 1/z, s, and t and clamp
	du = (float)u;
	dv = (float)v;

	sdivz = map->sdivzorigin + dv*map->sdivzstepv + du*map->sdivzstepu;
	tdivz = map->tdivzorigin + dv*map->tdivzstepv + du*map->tdivzstepu;
	zi = map->ziorigin + dv*map->zistepv + du*map->zistepu;
	z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
	pixelzi = zi;

	s = (int)(sdivz * z) + map->sadjust;
	if (s > map->bbextents)
		s = map->bbextents;
	else if (s < 0)
		s = 0;

	t = (int)(tdivz * z) + map->tadjust;
	if (t > map->bbextentt)
		t = map->bbextentt;
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

			snext = (int)(sdivz * z) + map->sadjust;
			if (snext > map->bbextents)
				snext = map->bbextents;
			else if (snext < 8)
				snext = 8;	// prevent round-off error on <0 steps from
							//  from causing overstepping & running off the
							//  edge of the texture

			tnext = (int)(tdivz * z) + map->tadjust;
			if (tnext > map->bbextentt)
				tnext = map->bbextentt;
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
			sdivz += map->sdivzstepu * spancountminus1;
			tdivz += map->tdivzstepu * spancountminus1;
			zi += map->zistepu * spancountminus1;
			z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
			snext = (int)(sdivz * z) + map->sadjust;
			if (snext > map->bbextents)
				snext = map->bbextents;
			else if (snext < 8)
				snext = 8;	// prevent round-off error on <0 steps from
							//  from causing overstepping & running off the
							//  edge of the texture

			tnext = (int)(tdivz * z) + map->tadjust;
			if (tnext > map->bbextentt)
				tnext = map->bbextentt;
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
			btemp = *(pbase + (s >> 16) + (t >> 16) * map->width);
			if (btemp != 255)
			{
				if (*pz <= pixelzi)
				{
					*pz = (float)pixelzi;
					*pdest = d_pal30_unlit[btemp];
				}
			}

			pixelzi += map->zistepu;
			pdest++;
			pz++;
			s += sstep;
			t += tstep;
		} while (--spancount > 0);

		s = snext;
		t = tnext;

	} while (count > 0);
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
	d_spritemap_t	map;
	sspan_t			*pspan;
	int				index;

	cachewidth = r_spritedesc.pspriteframe->width;
	sprite_height = r_spritedesc.pspriteframe->height;

	D_SpriteCalculateGradients ();
	if (!D_PolygonSpans (r_spritedesc.pverts, r_spritedesc.nump, sprite_spans, &r_refdef.vrect))
		return;

	map = (d_spritemap_t){
		.pixels = r_spritedesc.pspriteframe->pixels, .width = cachewidth,
		.sdivzorigin = d_sdivzorigin, .sdivzstepu = d_sdivzstepu, .sdivzstepv = d_sdivzstepv,
		.tdivzorigin = d_tdivzorigin, .tdivzstepu = d_tdivzstepu, .tdivzstepv = d_tdivzstepv,
		.ziorigin = d_ziorigin, .zistepu = d_zistepu, .zistepv = d_zistepv,
		.sadjust = sadjust, .tadjust = tadjust, .bbextents = bbextents, .bbextentt = bbextentt,
	};
	index = D_Keeping () ? D_KeepSpriteMap (&map) : -1;
	for (pspan = sprite_spans ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		if (pspan->count <= 0)
			continue;
		if (index >= 0)
			D_KeepSpriteSpan (pspan->u, pspan->v, pspan->count, index);
		else
			D_SpriteSpan (&map, pspan->u, pspan->v, pspan->count);
	}
}

/*
=====================
D_FenceSpan

A span of a fence on line v, from its cache block: depth tested and depth
written, the cut-out texels skipped
=====================
*/
void D_FenceSpan (const d_fencemap_t *map, int u, int v, int count)
{
	int			spancount;
	float		pixelzi;
	pixel_t		*pdest, texel;
	fixed16_t	s, t, snext, tnext, sstep, tstep;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float		sdivz8stepu, tdivz8stepu, zi8stepu;
	float		*pz;

	sstep = 0;
	tstep = 0;
	if (count <= 0)
		return;

	sdivz8stepu = map->sdivzstepu * 8;
	tdivz8stepu = map->tdivzstepu * 8;
	zi8stepu = map->zistepu * 8;

	pdest = d_viewbuffer + (screenwidth * v) + u;
	pz = d_pzbuffer + (d_zwidth * v) + u;

	// the initial s/z, t/z, 1/z, s and t, clamped
	du = (float)u;
	dv = (float)v;

	sdivz = map->sdivzorigin + dv*map->sdivzstepv + du*map->sdivzstepu;
	tdivz = map->tdivzorigin + dv*map->tdivzstepv + du*map->tdivzstepu;
	zi = map->ziorigin + dv*map->zistepv + du*map->zistepu;
	z = (float)0x10000 / zi;
	pixelzi = zi;

	s = (int)(sdivz * z) + map->sadjust;
	s = s > map->bbextents ? map->bbextents : (s < 0 ? 0 : s);
	t = (int)(tdivz * z) + map->tadjust;
	t = t > map->bbextentt ? map->bbextentt : (t < 0 ? 0 : t);

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
			snext = (int)(sdivz * z) + map->sadjust;
			snext = snext > map->bbextents ? map->bbextents : (snext < 8 ? 8 : snext);
			tnext = (int)(tdivz * z) + map->tadjust;
			tnext = tnext > map->bbextentt ? map->bbextentt : (tnext < 8 ? 8 : tnext);
			sstep = (snext - s) >> 3;
			tstep = (tnext - t) >> 3;
		}
		else
		{
			spancountminus1 = (float)(spancount - 1);
			sdivz += map->sdivzstepu * spancountminus1;
			tdivz += map->tdivzstepu * spancountminus1;
			zi += map->zistepu * spancountminus1;
			z = (float)0x10000 / zi;
			snext = (int)(sdivz * z) + map->sadjust;
			snext = snext > map->bbextents ? map->bbextents : (snext < 8 ? 8 : snext);
			tnext = (int)(tdivz * z) + map->tadjust;
			tnext = tnext > map->bbextentt ? map->bbextentt : (tnext < 8 ? 8 : tnext);
			if (spancount > 1)
			{
				sstep = (snext - s) / (spancount - 1);
				tstep = (tnext - t) / (spancount - 1);
			}
		}

		do
		{
			texel = map->block[(s >> 16) + (t >> 16) * map->width];
			if (!(texel & PIXEL_TRANSPARENT) && *pz <= pixelzi)
			{
				*pz = pixelzi;
				*pdest = texel;
			}
			pixelzi += map->zistepu;
			pdest++;
			pz++;
			s += sstep;
			t += tstep;
		} while (--spancount > 0);

		s = snext;
		t = tnext;
	} while (count > 0);
}

/*
=====================
D_DrawFencePolygon
=====================
*/
void D_DrawFencePolygon (emitpoint_t *pverts, int nump)
{
	d_fencemap_t	map;
	sspan_t			*pspan;
	int				index;

	if (!D_PolygonSpans (pverts, nump, sprite_spans, &r_refdef.vrect))
		return;
	map = (d_fencemap_t){
		.block = cacheblock, .width = cachewidth,
		.sdivzorigin = d_sdivzorigin, .sdivzstepu = d_sdivzstepu, .sdivzstepv = d_sdivzstepv,
		.tdivzorigin = d_tdivzorigin, .tdivzstepu = d_tdivzstepu, .tdivzstepv = d_tdivzstepv,
		.ziorigin = d_ziorigin, .zistepu = d_zistepu, .zistepv = d_zistepv,
		.sadjust = sadjust, .tadjust = tadjust, .bbextents = bbextents, .bbextentt = bbextentt,
	};
	index = D_Keeping () ? D_KeepFenceMap (&map) : -1;
	for (pspan = sprite_spans ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		if (pspan->count <= 0)
			continue;
		if (index >= 0)
			D_KeepFenceSpan (pspan->u, pspan->v, pspan->count, index);
		else
			D_FenceSpan (&map, pspan->u, pspan->v, pspan->count);
	}
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
	d_flatmap_t	map;
	float		zistepu, zistepv, ziorigin, area, det, best;
	float		du1, dv1, du2, dv2;
	int			i, j, index;

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
	map = (d_flatmap_t){.ziorigin = ziorigin, .zistepu = zistepu, .zistepv = zistepv, .color = color, .alpha = alpha};
	index = D_Keeping () ? D_KeepFlatMap (&map) : -1;
	for (pspan = sprite_spans ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		if (pspan->count <= 0)
			continue;
		if (index >= 0)
			D_KeepFlatSpan (pspan->u, pspan->v, pspan->count, index);
		else
			D_FlatSpan (&map, pspan->u, pspan->v, pspan->count);
	}
}

/*
=====================
D_FlatSpan

A span of a flat polygon on line v, blended in where it's in front
=====================
*/
void D_FlatSpan (const d_flatmap_t *map, int u, int v, int count)
{
	pixel_t	*pdest = d_viewbuffer + (screenwidth * v) + u;
	float	*pz = d_pzbuffer + (d_zwidth * v) + u;
	float	zi = map->ziorigin + v * map->zistepv + u * map->zistepu;

	for ( ; count ; count--, pdest++, pz++, zi += map->zistepu)
		if (*pz <= zi)
			*pdest = D_BlendPixel (map->color, *pdest, map->alpha);
}

