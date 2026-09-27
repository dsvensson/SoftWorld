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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "r_local.h"
#include "d_local.h"

static pixel_t	**warp_rowptr;		// the source rows, compressed by the wave's height
static int		*warp_column;		// and columns
static int		*warp_uturb;		// the wave's offset of each column
static int		*warp_vturb;		// and row

/*
=============
D_SetWarpSize

The underwater view is drawn into r_warpbuffer, then warped to the frame
=============
*/
void D_SetWarpSize (int width, int height, int scale)
{
	int		amp = AMP2 * scale;

	Mem_Free (r_warpbuffer);
	Mem_Free (warp_rowptr);
	Mem_Free (warp_column);
	Mem_Free (warp_uturb);
	Mem_Free (warp_vturb);
	r_warpbuffer = Mem_Alloc ((size_t)width * height * sizeof(*r_warpbuffer));
	warp_rowptr = Mem_Alloc ((size_t)(height + amp * 2) * sizeof(*warp_rowptr));
	warp_column = Mem_Alloc ((size_t)(width + amp * 2) * sizeof(*warp_column));
	warp_uturb = Mem_Alloc ((size_t)width * sizeof(*warp_uturb));
	warp_vturb = Mem_Alloc ((size_t)height * sizeof(*warp_vturb));
}

/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
=============
*/
void D_WarpScreen (void)
{
	int		w, h, u, v, k, amp, base;
	pixel_t	*dest;
	int		*col;
	pixel_t	**row;

	w = r_refdef.vrect.width;
	h = r_refdef.vrect.height;
	k = (int)vid.scale;
	amp = AMP2 * k;

	for (v=0 ; v<h+amp*2 ; v++)
	{
		warp_rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
				 (screenwidth * (int)((float)v * h / (h + amp * 2)));
	}

	for (u=0 ; u<w+amp*2 ; u++)
	{
		warp_column[u] = r_refdef.vrect.x +
				(int)((float)u * w / (w + amp * 2));
	}

// the 320x200 look: the wave is k pixels wide and moves k pixels at a time
	base = (int)(r_scene.time*SPEED)&(CYCLE-1);
	for (u=0 ; u<w ; u++)
		warp_uturb[u] = k * intsintable[base + u / k];
	for (v=0 ; v<h ; v++)
		warp_vturb[v] = k * intsintable[base + v / k];

	dest = vid.buffer + r_viewrect.y * vid.rowpixels + r_viewrect.x;

	for (v=0 ; v<r_viewrect.height ; v++, dest += vid.rowpixels)
	{
		col = &warp_column[warp_vturb[v]];
		row = &warp_rowptr[v];
		for (u=0 ; u<r_viewrect.width ; u++)
			dest[u] = row[warp_uturb[u]][col[u]];
	}
}



/*
=============
D_SpanTexmap

The current surface's texture mapping, for the span kernels
=============
*/
static simd_texmap_t D_SpanTexmap (void)
{
	return (simd_texmap_t){
		.sdivzorigin = d_sdivzorigin, .sdivzstepu = d_sdivzstepu, .sdivzstepv = d_sdivzstepv,
		.tdivzorigin = d_tdivzorigin, .tdivzstepu = d_tdivzstepu, .tdivzstepv = d_tdivzstepv,
		.ziorigin = d_ziorigin, .zistepu = d_zistepu, .zistepv = d_zistepv,
		.sadjust = sadjust, .tadjust = tadjust,
		.sextent = bbextents, .textent = bbextentt,
	};
}


/*
=============
Turbulent8
=============
*/
void Turbulent8 (espan_t *pspan)
{
	simd_texmap_t	map = D_SpanTexmap ();
	const int		*turb = sintable + ((int)(r_scene.time*SPEED)&(CYCLE-1));

	do
	{
		simd_turbspan (d_viewbuffer + (screenwidth * pspan->v) + pspan->u, &map, d_turbsource, d_pal30, turb,
			pspan->u, pspan->v, pspan->count);
	} while ((pspan = pspan->pnext) != NULL);
}


/*
=============
D_BlendRow
=============
*/
static pixel_t	*d_blendrow;
static int		d_blendrowsize;

pixel_t *D_BlendRow (int count)
{
	if (count > d_blendrowsize)
	{
		d_blendrowsize = count > 1024 ? count : 1024;
		d_blendrow = Mem_Realloc (d_blendrow, (size_t)d_blendrowsize * sizeof(*d_blendrow));
	}
	return d_blendrow;
}

/*
=============
D_DrawBlendedSpans

Spans of a translucent surface: each texel mapped as D_DrawSpans or
Turbulent8 maps it, then blended in where it isn't behind the depth buffer;
the depth buffer keeps what is behind
=============
*/
void D_DrawBlendedSpans (sspan_t *pspan, int alpha, bool turb)
{
	simd_texmap_t	map = D_SpanTexmap ();
	const int		*turbtab = sintable + ((int)(r_scene.time*SPEED)&(CYCLE-1));
	pixel_t			*pdest, *row, texel;
	float			*pz, zi;
	int				i;

	for ( ; pspan->count != DS_SPAN_LIST_END ; pspan++)
	{
		if (pspan->count <= 0)
			continue;
		row = D_BlendRow (pspan->count);
		if (turb)
			simd_turbspan (row, &map, d_turbsource, d_pal30, turbtab, pspan->u, pspan->v, pspan->count);
		else
			simd_texspan (row, &map, cacheblock, cachewidth, pspan->u, pspan->v, pspan->count);

		pdest = d_viewbuffer + screenwidth * pspan->v + pspan->u;
		pz = d_pzbuffer + d_zwidth * pspan->v + pspan->u;
		zi = d_ziorigin + pspan->v * d_zistepv + pspan->u * d_zistepu;
		for (i = 0 ; i < pspan->count ; i++, zi += d_zistepu)
		{
			texel = row[i];
			if (!(texel & PIXEL_TRANSPARENT) && pz[i] <= zi)
				pdest[i] = D_BlendPixel (texel, pdest[i], alpha);
		}
	}
}

/*
=============
D_DrawSpans
=============
*/
void D_DrawSpans (espan_t *pspan)
{
	simd_texmap_t	map = D_SpanTexmap ();

	do
	{
		simd_texspan (d_viewbuffer + (screenwidth * pspan->v) + pspan->u, &map, cacheblock, cachewidth,
			pspan->u, pspan->v, pspan->count);
	} while ((pspan = pspan->pnext) != NULL);
}


/*
=============
D_DrawZSpans
=============
*/
void D_DrawZSpans (espan_t *pspan)
{
	float	zi, du, dv;

	do
	{
	// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;

		simd_zspan (d_pzbuffer + (d_zwidth * pspan->v) + pspan->u, pspan->count, zi, d_zistepu);
	} while ((pspan = pspan->pnext) != NULL);
}

