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
// d_polyset.c: routines for drawing sets of polygons sharing the same
// texture (used for Alias models)

#include "r_local.h"
#include "d_local.h"


// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct {
	pixel_t			*pdest;
	float			*pz;
	int				count;
	byte			*ptex;
	int				sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
	int		isflattop;
	int		numleftedges;
	int		*pleftedgevert0;
	int		*pleftedgevert1;
	int		*pleftedgevert2;
	int		numrightedges;
	int		*prightedgevert0;
	int		*prightedgevert1;
	int		*prightedgevert2;
} edgetable;

static int	r_p0[6], r_p1[6], r_p2[6];

static int		d_tlight;		// the light of the triangle drawn by subdivision

int			d_aflatcolor;
static int			d_xdenom;

static edgetable	*pedgetable;

static edgetable	edgetables[12] = {
	{0, 1, r_p0, r_p2, NULL, 2, r_p0, r_p1, r_p2 },
	{0, 2, r_p1, r_p0, r_p2,   1, r_p1, r_p2, NULL},
	{1, 1, r_p0, r_p2, NULL, 1, r_p1, r_p2, NULL},
	{0, 1, r_p1, r_p0, NULL, 2, r_p1, r_p2, r_p0 },
	{0, 2, r_p0, r_p2, r_p1,   1, r_p0, r_p1, NULL},
	{0, 1, r_p2, r_p1, NULL, 1, r_p2, r_p0, NULL},
	{0, 1, r_p2, r_p1, NULL, 2, r_p2, r_p0, r_p1 },
	{0, 2, r_p2, r_p1, r_p0,   1, r_p2, r_p0, NULL},
	{0, 1, r_p1, r_p0, NULL, 1, r_p1, r_p2, NULL},
	{1, 1, r_p2, r_p1, NULL, 1, r_p0, r_p1, NULL},
	{1, 1, r_p1, r_p0, NULL, 1, r_p2, r_p0, NULL},
	{0, 1, r_p0, r_p2, NULL, 1, r_p0, r_p1, NULL},
};

// FIXME: some of these can become statics
static int				a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
static int				r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
static int				r_zistepx, r_zistepy;
static int				d_aspancount, d_countextrastep;

static int				d_atoprow;		// the triangle's top row, a_spans[0]'s

static spanpackage_t			*a_spans;
static spanpackage_t			*d_pedgespanpackage;
static int				ystart;
static pixel_t				*d_pdest;
static byte					*d_ptex;
static float					*d_pz;
static int						d_sfrac, d_tfrac, d_light, d_zi;
static int						d_ptexextrastep, d_sfracextrastep;
static int						d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
static int						d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
static int						d_sfracbasestep, d_tfracbasestep;
static int						d_ziextrastep, d_zibasestep;
static int						d_pzextrastep, d_pzbasestep;

typedef struct {
	int		quotient;
	int		remainder;
} adivtab_t;

static adivtab_t	adivtab[32*32] = {
#include "adivtab.inc"
};

/*
================
D_SkinTexel

The skin's texel at 16.16 s and t
================
*/
static inline int D_SkinTexel (int s, int t)
{
	return ((const byte *)r_affinetridesc.pskin)[(t >> 16) * r_affinetridesc.skinwidth + (s >> 16)];
}

// a texel nothing is drawn at, nor its depth (MF_HOLEY)
static inline bool D_SkinHole (int texel)
{
	return r_affinetridesc.holey && texel == 255;
}

/*
================
D_AliasBlendSpan

A span of a translucent model, alpha of 256: its texels depth tested and
depth written as usual, into a row that is then fogged and blended into the
frame. Any thread's, kept spans' too (d_batch.c).
================
*/
void D_AliasBlendSpan (pixel_t *pdest, float *pz, const byte *ptex, int sfrac, int tfrac, int light, int zi,
	int count, const simd_aliasmap_t *map, int alpha)
{
	pixel_t	*row = D_BlendRow (count);
	int		i;

	for (i=0 ; i<count ; i++)
		row[i] = 0xffffffffu;		// no texel: its top bit, which no pixel has
	simd_aliasspan (row, pz, ptex, sfrac, tfrac, light, zi, count, map);
	if (r_fogactive)
		simd_fogspan (row, NULL, (float)zi * ALIAS_ZI_TO_FLOAT, (float)map->zistep * ALIAS_ZI_TO_FLOAT,
			count, &d_fog);
	simd_blendspan (pdest, row, NULL, 0, 0, alpha, count);
}

/*
================
D_AliasBlendPixel

A pixel of a translucent model, 1/z z, depth tested and written, fogged and
blended; any thread's, as D_AliasBlendSpan
================
*/
void D_AliasBlendPixel (pixel_t *dest, float *pz, pixel_t color, float z, int alpha)
{
	if (z < *pz)
		return;
	*pz = z;
	if (r_fogactive)
		color = R_FogPixel (color, z);
	*dest = D_BlendPixel (color, *dest, alpha);
}

static void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage);
static void D_PolysetCalcGradients (int skinw);
static void D_DrawSubdiv (void);
static void D_DrawNonSubdiv (void);
static void D_PolysetRecursiveTriangle (int *p1, int *p2, int *p3);
static void D_PolysetSetEdgeTable (void);
static void D_RasterizeAliasPolySmooth (void);
static void D_PolysetScanLeftEdge (int height);


/*
================
D_SetPolysetSize

A span for every scan line, one to mark the end and one more because of
cache line pretouching
================
*/
void D_SetPolysetSize (int height)
{
	Mem_Free (a_spans);
	a_spans = Mem_Calloc ((size_t)height + 3, sizeof(*a_spans));
}

/*
================
D_PolysetDraw
================
*/
void D_PolysetDraw (void)
{
	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdiv ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
}


/*
================
D_AliasPixel

A skin texel lit: through the colormap row of its light, or with
r_lightmode 1 its color times the light in the light's color. light is
(255 - level) << 6, level 128 being 1.0.
================
*/
static inline pixel_t D_AliasPixel (int index, int light)
{
	unsigned	level;

	if (!r_affinetridesc.rgblight)
		return d_cm30[index + (light & 0xFF00)];
	level = light < (255 << 6) ? (unsigned)((255 << 6) - light) : 0;	// 8192 is 1.0
	return R_LitColor (r_affinetridesc.palette[index], d_pal30_floor[index], (level * r_affinetridesc.tint[0]) >> 6,
		(level * r_affinetridesc.tint[1]) >> 6, (level * r_affinetridesc.tint[2]) >> 6);
}

int		d_alpha = 256;

/*
================
D_AliasPut

A single pixel of a model, 1/z zi, blended if it is translucent: drawn after
the fog, it is fogged first
================
*/
static void D_AliasPut (pixel_t *dest, pixel_t color, float zi)
{
	if (d_alpha >= 256)
	{
		*dest = color;
		return;
	}
	if (r_fogactive)
		color = R_FogPixel (color, zi);
	*dest = D_BlendPixel (color, *dest, d_alpha);
}

/*
================
D_AliasPoint

A pixel of a model at u, v, depth tested at z: kept in the batch, or drawn
================
*/
static void D_AliasPoint (int u, int v, float z, pixel_t color)
{
	pixel_t	*dest = &d_viewbuffer[d_scantable[v] + u];
	float	*zbuf = zspantable[v] + u;

	if (D_Keeping ())
	{
		D_KeepAliasPixel (v, dest, zbuf, color, z, d_alpha);
		return;
	}
	if (z >= *zbuf)
	{
		*zbuf = z;
		D_AliasPut (dest, color, z);
	}
}

/*
================
D_PolysetDrawFinalVerts
================
*/
void D_PolysetDrawFinalVerts (finalvert_t *fv, int nverts)
{
	int		i, pix;

	for (i=0 ; i<nverts ; i++, fv++)
	{
	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
		if ((fv->v[0] < r_refdef.vrectright) &&
			(fv->v[1] < r_refdef.vrectbottom))
		{
			pix = D_SkinTexel (fv->v[2], fv->v[3]);
			if (!D_SkinHole (pix))
				D_AliasPoint (fv->v[0], fv->v[1], fv->v[5] * ALIAS_ZI_TO_FLOAT,
					D_AliasPixel (r_affinetridesc.skinremap[pix], fv->v[4]));
		}
	}
}


/*
================
D_DrawSubdiv
================
*/
static void D_DrawSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int				i;
	int				lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i=0 ; i<lnumtriangles ; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) *
			 (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * 
			 (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}

		d_tlight = index0->v[4];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}


/*
================
D_DrawNonSubdiv
================
*/
static void D_DrawNonSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int				i;
	int				lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i=0 ; i<lnumtriangles ; i++, ptri++)
	{
		index0 = pfv + ptri->vertindex[0];
		index1 = pfv + ptri->vertindex[1];
		index2 = pfv + ptri->vertindex[2];

		d_xdenom = (index0->v[1]-index1->v[1]) *
				(index0->v[0]-index2->v[0]) -
				(index0->v[0]-index1->v[0])*(index0->v[1]-index2->v[1]);

		if (d_xdenom >= 0)
		{
			continue;
		}

		r_p0[0] = index0->v[0];		// u
		r_p0[1] = index0->v[1];		// v
		r_p0[2] = index0->v[2];		// s
		r_p0[3] = index0->v[3];		// t
		r_p0[4] = index0->v[4];		// light
		r_p0[5] = index0->v[5];		// iz

		r_p1[0] = index1->v[0];
		r_p1[1] = index1->v[1];
		r_p1[2] = index1->v[2];
		r_p1[3] = index1->v[3];
		r_p1[4] = index1->v[4];
		r_p1[5] = index1->v[5];

		r_p2[0] = index2->v[0];
		r_p2[1] = index2->v[1];
		r_p2[2] = index2->v[2];
		r_p2[3] = index2->v[3];
		r_p2[4] = index2->v[4];
		r_p2[5] = index2->v[5];

		if (!ptri->facesfront)
		{
			if (index0->flags & ALIAS_ONSEAM)
				r_p0[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				r_p1[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				r_p2[2] += r_affinetridesc.seamfixupX16;
		}

		D_PolysetSetEdgeTable ();
		D_RasterizeAliasPolySmooth ();
	}
}


/*
================
D_PolysetRecursiveTriangle
================
*/
static void D_PolysetRecursiveTriangle (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d, pix;
	int		new[6];

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return;			// entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new[0] = (lp1[0] + lp2[0]) >> 1;
	new[1] = (lp1[1] + lp2[1]) >> 1;
	new[2] = (lp1[2] + lp2[2]) >> 1;
	new[3] = (lp1[3] + lp2[3]) >> 1;
	new[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;


	pix = D_SkinTexel (new[2], new[3]);
	if (!D_SkinHole (pix))
		D_AliasPoint (new[0], new[1], new[5] * ALIAS_ZI_TO_FLOAT,
			D_AliasPixel (r_affinetridesc.skinremap[pix], d_tlight));

nodraw:
// recursively continue
	D_PolysetRecursiveTriangle (lp3, lp1, new);
	D_PolysetRecursiveTriangle (lp3, new, lp2);
}



// a + step * count as the machine computes it, wrapping: a sliver of a
// triangle has steps whose sums overflow an int, which C leaves undefined (the
// span kernels wrap theirs too)
static inline int D_WrapStep (int a, int step, int count)
{
	return (int)((unsigned)a + (unsigned)step * (unsigned)count);
}

/*
===================
D_PolysetSetUpLeftEdgeSteps

The left edge's steps along ubasestep, and one pixel further for when the
error term carries
====================
*/
static void D_PolysetSetUpLeftEdgeSteps (void)
{
	int		working_lstepx, s, t;

// for negative steps in x along left edge, bias toward overflow rather than
// underflow (sort of turning the floor () we did in the gradient calcs into
// ceil (), but plus a little bit)
	if (ubasestep < 0)
		working_lstepx = D_WrapStep (r_lstepx, -1, 1);
	else
		working_lstepx = r_lstepx;

	d_countextrastep = ubasestep + 1;
	s = D_WrapStep (r_sstepy, r_sstepx, ubasestep);
	t = D_WrapStep (r_tstepy, r_tstepx, ubasestep);
	d_ptexbasestep = (s >> 16) + (t >> 16) * r_affinetridesc.skinwidth;
	d_sfracbasestep = s & 0xFFFF;
	d_tfracbasestep = t & 0xFFFF;
	d_lightbasestep = D_WrapStep (r_lstepy, working_lstepx, ubasestep);
	d_zibasestep = D_WrapStep (r_zistepy, r_zistepx, ubasestep);

	s = D_WrapStep (r_sstepy, r_sstepx, d_countextrastep);
	t = D_WrapStep (r_tstepy, r_tstepx, d_countextrastep);
	d_ptexextrastep = (s >> 16) + (t >> 16) * r_affinetridesc.skinwidth;
	d_sfracextrastep = s & 0xFFFF;
	d_tfracextrastep = t & 0xFFFF;
	d_lightextrastep = D_WrapStep (d_lightbasestep, working_lstepx, 1);
	d_ziextrastep = D_WrapStep (d_zibasestep, r_zistepx, 1);
}

/*
===================
D_PolysetScanLeftEdge
====================
*/
static void D_PolysetScanLeftEdge (int height)
{

	do
	{
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;

		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;

	// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;

		d_pedgespanpackage++;

		errorterm += erroradjustup;
		if (errorterm >= 0)
		{
			d_pdest += d_pdestextrastep;
			d_pz += d_pzextrastep;
			d_aspancount += d_countextrastep;
			d_ptex += d_ptexextrastep;
			d_sfrac += d_sfracextrastep;
			d_ptex += d_sfrac >> 16;

			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracextrastep;
			if (d_tfrac & 0x10000)
			{
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light = D_WrapStep (d_light, d_lightextrastep, 1);
			d_zi = D_WrapStep (d_zi, d_ziextrastep, 1);
			errorterm -= erroradjustdown;
		}
		else
		{
			d_pdest += d_pdestbasestep;
			d_pz += d_pzbasestep;
			d_aspancount += ubasestep;
			d_ptex += d_ptexbasestep;
			d_sfrac += d_sfracbasestep;
			d_ptex += d_sfrac >> 16;
			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracbasestep;
			if (d_tfrac & 0x10000)
			{
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light = D_WrapStep (d_light, d_lightbasestep, 1);
			d_zi = D_WrapStep (d_zi, d_zibasestep, 1);
		}
	} while (--height);
}



/*
===================
D_PolysetSetUpForLineScan
====================
*/
static void D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
		fixed8_t endvertu, fixed8_t endvertv)
{
	double		dm, dn;
	int			tm, tn;
	adivtab_t	*ptemp;

// TODO: implement x86 version

	errorterm = -1;

	tm = endvertu - startvertu;
	tn = endvertv - startvertv;

	if (((tm <= 16) && (tm >= -15)) &&
		((tn <= 16) && (tn >= -15)))
	{
		ptemp = &adivtab[((tm+15) << 5) + (tn+15)];
		ubasestep = ptemp->quotient;
		erroradjustup = ptemp->remainder;
		erroradjustdown = tn;
	}
	else
	{
		dm = (double)tm;
		dn = (double)tn;

		FloorDivMod (dm, dn, &ubasestep, &erroradjustup);

		erroradjustdown = (int)dn;
	}
}



/*
================
D_PolysetCalcGradients
================
*/
static void D_PolysetCalcGradients (int skinw)
{
	float	xstepdenominv, ystepdenominv, t0, t1;
	float	p01_minus_p21, p11_minus_p21, p00_minus_p20, p10_minus_p20;

	p00_minus_p20 = (float)(r_p0[0] - r_p2[0]);
	p01_minus_p21 = (float)(r_p0[1] - r_p2[1]);
	p10_minus_p20 = (float)(r_p1[0] - r_p2[0]);
	p11_minus_p21 = (float)(r_p1[1] - r_p2[1]);

	xstepdenominv = 1.0f / (float)d_xdenom;

	ystepdenominv = -xstepdenominv;

// ceil () for light so positive steps are exaggerated, negative steps
// diminished,  pushing us away from underflow toward overflow. Underflow is
// very visible, overflow is very unlikely, because of ambient lighting
	t0 = (float)(r_p0[4] - r_p2[4]);
	t1 = (float)(r_p1[4] - r_p2[4]);
	r_lstepx = R_SaturateInt (
			ceil((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv));
	r_lstepy = R_SaturateInt (
			ceil((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv));

	// a sliver's steps can be past an int's range: saturated
	t0 = (float)(r_p0[2] - r_p2[2]);
	t1 = (float)(r_p1[2] - r_p2[2]);
	r_sstepx = R_SaturateInt ((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_sstepy = R_SaturateInt ((t1 * p00_minus_p20 - t0* p10_minus_p20) *
			ystepdenominv);

	t0 = (float)(r_p0[3] - r_p2[3]);
	t1 = (float)(r_p1[3] - r_p2[3]);
	r_tstepx = R_SaturateInt ((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_tstepy = R_SaturateInt ((t1 * p00_minus_p20 - t0 * p10_minus_p20) *
			ystepdenominv);

	t0 = (float)(r_p0[5] - r_p2[5]);
	t1 = (float)(r_p1[5] - r_p2[5]);
	r_zistepx = R_SaturateInt ((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_zistepy = R_SaturateInt ((t1 * p00_minus_p20 - t0 * p10_minus_p20) *
			ystepdenominv);

	a_sstepxfrac = r_sstepx & 0xFFFF;
	a_tstepxfrac = r_tstepx & 0xFFFF;

	a_ststepxwhole = skinw * (r_tstepx >> 16) + (r_sstepx >> 16);
}




/*
================
D_PolysetDrawSpans8
================
*/
static void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage)
{
	int				lcount;
	simd_aliasmap_t	map = {
		.zistep = r_zistepx, .lightstep = r_lstepx,
		.stepwhole = a_ststepxwhole, .sfracstep = a_sstepxfrac, .tfracstep = a_tstepxfrac,
		.skinwidth = r_affinetridesc.skinwidth,
		.remap = r_affinetridesc.skinremap,
		.colormap = r_affinetridesc.rgblight ? NULL : d_cm30,
		.palette = r_affinetridesc.palette, .floor = d_pal30_floor,
		.tint = {r_affinetridesc.tint[0], r_affinetridesc.tint[1], r_affinetridesc.tint[2]},
		.holey = r_affinetridesc.holey,
	};
	int				mapindex = -1;		// the map's in the batch, once a span is kept
	bool			keeping = D_Keeping ();

	do
	{
		lcount = d_aspancount - pspanpackage->count;

		errorterm += erroradjustup;
		if (errorterm >= 0)
		{
			d_aspancount += d_countextrastep;
			errorterm -= erroradjustdown;
		}
		else
		{
			d_aspancount += ubasestep;
		}

		if (!lcount)
			;
		else if (keeping)
		{
			// a blend too: it reads what's under it when its strip is drawn,
			// all kept before it drawn by then
			if (mapindex < 0)
				mapindex = D_KeepAliasMap (&map);
			D_KeepAliasSpan (d_atoprow + (int)(pspanpackage - a_spans), pspanpackage->pdest, pspanpackage->pz,
				pspanpackage->ptex, pspanpackage->sfrac, pspanpackage->tfrac, pspanpackage->light,
				pspanpackage->zi, lcount, mapindex, d_alpha);
		}
		else if (d_alpha < 256)
			D_AliasBlendSpan (pspanpackage->pdest, pspanpackage->pz, pspanpackage->ptex, pspanpackage->sfrac,
				pspanpackage->tfrac, pspanpackage->light, pspanpackage->zi, lcount, &map, d_alpha);
		else
			simd_aliasspan (pspanpackage->pdest, pspanpackage->pz, pspanpackage->ptex, pspanpackage->sfrac,
				pspanpackage->tfrac, pspanpackage->light, pspanpackage->zi, lcount, &map);

		pspanpackage++;
	} while (pspanpackage->count != -999999);
}

/*
================
D_RasterizeAliasPolySmooth
================
*/
static void D_RasterizeAliasPolySmooth (void)
{
	int				initialleftheight, initialrightheight;
	int				*plefttop, *prighttop, *pleftbottom, *prightbottom;
	int				originalcount;

	plefttop = pedgetable->pleftedgevert0;
	prighttop = pedgetable->prightedgevert0;

	pleftbottom = pedgetable->pleftedgevert1;
	prightbottom = pedgetable->prightedgevert1;

	initialleftheight = pleftbottom[1] - plefttop[1];
	initialrightheight = prightbottom[1] - prighttop[1];

//
// set the s, t, and light gradients, which are consistent across the triangle
// because being a triangle, things are affine
//
	D_PolysetCalcGradients (r_affinetridesc.skinwidth);

//
// rasterize the polygon
//

//
// scan out the top (and possibly only) part of the left edge
//
	D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
						  pleftbottom[0], pleftbottom[1]);

	d_pedgespanpackage = a_spans;

	ystart = plefttop[1];
	d_atoprow = ystart;		// a_spans go on down from it, both left edges'
	d_aspancount = plefttop[0] - prighttop[0];

	d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
			(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
	d_sfrac = plefttop[2] & 0xFFFF;
	d_tfrac = plefttop[3] & 0xFFFF;
	d_pzbasestep = d_zwidth + ubasestep;
	d_pzextrastep = d_pzbasestep + 1;
	d_light = plefttop[4];
	d_zi = plefttop[5];

	d_pdestbasestep = screenwidth + ubasestep;
	d_pdestextrastep = d_pdestbasestep + 1;
	d_pdest = d_viewbuffer + ystart * screenwidth + plefttop[0];
	d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

	D_PolysetSetUpLeftEdgeSteps ();

	D_PolysetScanLeftEdge (initialleftheight);

//
// scan out the bottom part of the left edge, if it exists
//
	if (pedgetable->numleftedges == 2)
	{
		int		height;

		plefttop = pleftbottom;
		pleftbottom = pedgetable->pleftedgevert2;

		D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
							  pleftbottom[0], pleftbottom[1]);

		height = pleftbottom[1] - plefttop[1];

// TODO: make this a function; modularize this function in general

		ystart = plefttop[1];
		d_aspancount = plefttop[0] - prighttop[0];
		d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
				(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
		d_sfrac = 0;
		d_tfrac = 0;
		d_light = plefttop[4];
		d_zi = plefttop[5];

		d_pdestbasestep = screenwidth + ubasestep;
		d_pdestextrastep = d_pdestbasestep + 1;
		d_pdest = d_viewbuffer + ystart * screenwidth + plefttop[0];
		d_pzbasestep = d_zwidth + ubasestep;
		d_pzextrastep = d_pzbasestep + 1;
		d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

		D_PolysetSetUpLeftEdgeSteps ();

		D_PolysetScanLeftEdge (height);
	}

// scan out the top (and possibly only) part of the right edge, updating the
// count field
	d_pedgespanpackage = a_spans;

	D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
						  prightbottom[0], prightbottom[1]);
	d_aspancount = 0;
	d_countextrastep = ubasestep + 1;
	originalcount = a_spans[initialrightheight].count;
	a_spans[initialrightheight].count = -999999; // mark end of the spanpackages
	D_PolysetDrawSpans8 (a_spans);

// scan out the bottom part of the right edge, if it exists
	if (pedgetable->numrightedges == 2)
	{
		int				height;
		spanpackage_t	*pstart;

		pstart = a_spans + initialrightheight;
		pstart->count = originalcount;

		d_aspancount = prightbottom[0] - prighttop[0];

		prighttop = prightbottom;
		prightbottom = pedgetable->prightedgevert2;

		height = prightbottom[1] - prighttop[1];

		D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
							  prightbottom[0], prightbottom[1]);

		d_countextrastep = ubasestep + 1;
		a_spans[initialrightheight + height].count = -999999;
											// mark end of the spanpackages
		D_PolysetDrawSpans8 (pstart);
	}
}


/*
================
D_PolysetSetEdgeTable
================
*/
static void D_PolysetSetEdgeTable (void)
{
	int			edgetableindex;

	edgetableindex = 0;	// assume the vertices are already in
						//  top to bottom order

//
// determine which edges are right & left, and the order in which
// to rasterize them
//
	if (r_p0[1] >= r_p1[1])
	{
		if (r_p0[1] == r_p1[1])
		{
			if (r_p0[1] < r_p2[1])
				pedgetable = &edgetables[2];
			else
				pedgetable = &edgetables[5];

			return;
		}
		else
		{
			edgetableindex = 1;
		}
	}

	if (r_p0[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[8];
		else
			pedgetable = &edgetables[9];

		return;
	}
	else if (r_p1[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[10];
		else
			pedgetable = &edgetables[11];

		return;
	}

	if (r_p0[1] > r_p2[1])
		edgetableindex += 2;

	if (r_p1[1] > r_p2[1])
		edgetableindex += 4;

	pedgetable = &edgetables[edgetableindex];
}



