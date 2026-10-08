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
// r_partdraw.c -- scripted particles (r_scene.particles) into the view.
//
// Each is a polygon or a line, made as QuakeSpasm-Spiked's are, clipped to the
// near plane and projected: an item. The items are set up in parallel, then
// sorted into bands of rows, and the bands filled in parallel; a band takes
// its items in the order given, so a pixel blends as GL would blend it. A
// pixel of an item adds light and multiplies the light under it, both in
// linear light (simd_partspan), as GL's blend factors would in its colors:
// darkening as GL darkens, blending and adding as light blends and adds, past
// white into HDR. Particles are depth tested and never depth written; fog
// fades each by its depth.

#include "r_local.h"
#include "d_local.h"

#include <stddef.h>

#define	PART_NEAR		4.0f			// GL's near plane, which cuts particles as near
#define	PART_MAXVERTS	8				// a quad cut by the near plane, and room
#define	PART_BAND		32				// rows a band
#define	PART_CHUNK		512				// particles a setup job
#define	PART_DECALBIAS	(1.0f / 1024)	// decals are nearer by this much, off the wall they lie on
#define	PART_MAXLINE	4096			// pixels a line at most

enum {ITEM_NONE, ITEM_POLY, ITEM_LINE};

// what a pixel adds and multiplies, by the blend
enum
{
	SHADE_LIGHT,		// light weighted by alpha, and the light under it by 1 - alpha or 1
	SHADE_INVMODA,		// the light under it darkened as GL darkens by alpha
	SHADE_INVMODC,		// and by color
	SHADE_ADDC,			// color times itself added
	SHADE_BLENDCOLOR,	// color blended by color
	SHADE_SUBTRACT		// color by alpha, the light under it by 1 - color
};

// a corner in view space (x right, y up, z ahead)
typedef struct
{
	float	x, y, z;
	float	s, t;
	float	rgba[4];
} partvert_t;

typedef struct
{
	byte	kind;
	byte	shade;
	byte	varying;			// its color is interpolated (decals, beams), else rgba
	byte	level;				// of its image
	byte	weightalpha;		// SHADE_LIGHT, SHADE_INVMODC: its color weighted by its alpha
	byte	covers;				// SHADE_LIGHT: the light under it by 1 - alpha, else 1
	const partimage_t	*image;	// NULL: white (lines)
	int		miny, maxy;			// the rows it covers, maxy past the last
	int		nv;
	float	u[PART_MAXVERTS], v[PART_MAXVERTS];
	float	plane[7][3];		// over the view, a*u + b*v + c: zi, s*zi, t*zi, and rgba*zi
	float	rgba[4];			// sRGB 0..1, alpha 0..1
	float	lin[3];				// its color as linear light, 0..1, unless varying
	float	fog;				// the light fog leaves it, 0..1
	float	bias;				// its depth as tested: 1, a decal's nearer
	float	lz[2], lrgba[2][4];	// a line's ends' 1/z and colors
} partitem_t;

static const r_partscene_t	*r_partscene;
static partitem_t	*r_partitems;
static int			r_maxpartitems;
static int			*r_partfirst;		// each batch's first item
static int			r_maxbatches;
static int			*r_partbands;		// a band's items, by index: bandstart[b] .. bandstart[b+1]
static int			*r_partbandstart;
static int			r_maxbandlist, r_maxbands;
static float		*r_partrows;		// each band's S and M rows: 6 rows of the view's width
static int			r_partrowsize, r_partrowbands;

// an sRGB value, 0 to 1, as linear light, 0 to 1
static float R_PartLinear (float c)
{
	int		i = (int)(c * 255 + 0.5f);

	return R_SrgbLightTable ()[i < 0 ? 0 : i > 255 ? 255 : i] * (1 / PART_WHITE);
}

// the light the fog leaves at 1/z zi, as simd_fogspan takes it
static float R_PartFog (float zi)
{
	int32_t	bits;
	int		e;

	if (!r_fogactive)
		return 1;
	memcpy (&bits, &zi, sizeof(bits));
	if (bits < 0)
		e = 0;
	else
	{
		e = (bits >> SIMD_FOG_SHIFT) - d_fog.base;
		e = e < 0 ? 0 : e < d_fog.size ? e : d_fog.size - 1;
	}
	return d_fog.table[e] * (1.0f / 256);
}

/*
==============================================================================

SETUP: particles into items

==============================================================================
*/

static void R_PartCorner (partvert_t *pv, const vec3_t p, float s, float t, const float *rgba)
{
	vec3_t	local;

	VectorSubtract (p, r_origin, local);
	pv->x = DotProduct (local, vright);
	pv->y = DotProduct (local, vup);
	pv->z = DotProduct (local, vpn);
	pv->s = s;
	pv->t = t;
	memcpy (pv->rgba, rgba, sizeof(pv->rgba));
}

// the part of in nearer than the near plane cut away, into out
static int R_PartClipNear (const partvert_t *in, int n, partvert_t *out)
{
	const float	*a, *b;
	float		frac;
	int			i, j, k, m;

	for (i = m = 0 ; i < n && m < PART_MAXVERTS - 1 ; i++)
	{
		j = (i + 1) % n;
		if (in[i].z >= PART_NEAR)
			out[m++] = in[i];
		if ((in[i].z >= PART_NEAR) != (in[j].z >= PART_NEAR))
		{
			frac = (PART_NEAR - in[i].z) / (in[j].z - in[i].z);
			a = &in[i].x;
			b = &in[j].x;
			for (k = 0 ; k < (int)(sizeof(partvert_t) / sizeof(float)) ; k++)
				(&out[m].x)[k] = a[k] + frac * (b[k] - a[k]);
			m++;
		}
	}
	return m;
}

// the planes of attributes values[k][i] over the corners
static bool R_PartPlanes (partitem_t *it, const float values[][PART_MAXVERTS], int numvalues)
{
	float	du1, dv1, du2, dv2, det, best = 0, d1, d2;
	int		i, j, k, bi = 1, bj = 2;

	for (i = 1 ; i < it->nv ; i++)
		for (j = i + 1 ; j < it->nv ; j++)
		{
			det = fabsf ((it->u[i] - it->u[0]) * (it->v[j] - it->v[0]) - (it->u[j] - it->u[0]) * (it->v[i] - it->v[0]));
			if (det > best)
			{
				best = det;
				bi = i;
				bj = j;
			}
		}
	if (best < 0.01f)
		return false;	// seen edge on
	du1 = it->u[bi] - it->u[0];
	dv1 = it->v[bi] - it->v[0];
	du2 = it->u[bj] - it->u[0];
	dv2 = it->v[bj] - it->v[0];
	det = du1 * dv2 - du2 * dv1;
	for (k = 0 ; k < numvalues ; k++)
	{
		d1 = values[k][bi] - values[k][0];
		d2 = values[k][bj] - values[k][0];
		it->plane[k][0] = (d1 * dv2 - d2 * dv1) / det;
		it->plane[k][1] = (du1 * d2 - du2 * d1) / det;
		it->plane[k][2] = values[k][0] - it->plane[k][0] * it->u[0] - it->plane[k][1] * it->v[0];
	}
	return true;
}

/*
=================
R_PartPolygon

A polygon item of corners in, clipped and projected; false if none of it shows
=================
*/
static bool R_PartPolygon (partitem_t *it, const partvert_t *in, int n)
{
	partvert_t	clipped[PART_MAXVERTS];
	float		values[7][PART_MAXVERTS], zi, minu, maxu, minv, maxv, tex, area, sumzi;
	int			i, k, j, level, top = r_refdef.vrect.y, bottom = r_refdef.vrect.y + r_refdef.vrect.height;

	it->nv = R_PartClipNear (in, n, clipped);
	if (it->nv < 3)
		return false;
	minu = minv = 1e30f;
	maxu = maxv = -1e30f;
	sumzi = 0;
	for (i = 0 ; i < it->nv ; i++)
	{
		zi = 1.0f / clipped[i].z;
		sumzi += zi;
		it->u[i] = xcenter + xscale * zi * clipped[i].x;
		it->v[i] = ycenter - yscale * zi * clipped[i].y;
		minu = fminf (minu, it->u[i]);
		maxu = fmaxf (maxu, it->u[i]);
		minv = fminf (minv, it->v[i]);
		maxv = fmaxf (maxv, it->v[i]);
		values[0][i] = zi;
		values[1][i] = clipped[i].s * zi;
		values[2][i] = clipped[i].t * zi;
		for (k = 0 ; k < 4 ; k++)
			values[3+k][i] = clipped[i].rgba[k] * zi;
	}
	if (maxu < r_refdef.vrect.x || minu >= r_refdef.vrect.x + r_refdef.vrect.width)
		return false;
	it->miny = (int)ceilf (fmaxf (minv, (float)top));
	it->maxy = (int)ceilf (fminf (maxv, (float)bottom));
	if (it->miny >= it->maxy)
		return false;
	if (!R_PartPlanes (it, (const float (*)[PART_MAXVERTS])values, it->varying ? 7 : 3))
		return false;
	it->fog = R_PartFog (sumzi / (float)it->nv);

	// the level of the image whose texels are nearest a pixel each
	it->level = 0;
	if (it->image)
	{
		tex = area = 0;
		for (i = 0 ; i < it->nv ; i++)
		{
			j = (i + 1) % it->nv;
			tex += clipped[i].s * clipped[j].t - clipped[j].s * clipped[i].t;
			area += it->u[i] * it->v[j] - it->u[j] * it->v[i];
		}
		tex = fabsf (tex) * (float)it->image->lw[0] * (float)it->image->lh[0];
		area = fabsf (area);
		level = 0;
		while (level < it->image->numlevels - 1 && tex > area * 2)
		{
			tex *= 0.25f;
			level++;
		}
		it->level = (byte)level;
	}
	for (k = 0 ; k < 3 ; k++)
		it->lin[k] = R_PartLinear (it->rgba[k]);
	it->kind = ITEM_POLY;
	return true;
}

/*
=================
R_PartLine

A line item from a to b, its colors fading from a's to b's
=================
*/
static bool R_PartLine (partitem_t *it, const partvert_t *a, const partvert_t *b)
{
	partvert_t	ends[2];
	float		frac, zi;
	int			i, k;

	ends[0] = *a;
	ends[1] = *b;
	if (a->z < PART_NEAR && b->z < PART_NEAR)
		return false;
	for (i = 0 ; i < 2 ; i++)
		if (ends[i].z < PART_NEAR)
		{
			frac = (PART_NEAR - ends[i].z) / (ends[!i].z - ends[i].z);
			for (k = 0 ; k < (int)(sizeof(partvert_t) / sizeof(float)) ; k++)
				(&ends[i].x)[k] += frac * ((&ends[!i].x)[k] - (&ends[i].x)[k]);
		}
	for (i = 0 ; i < 2 ; i++)
	{
		zi = 1.0f / ends[i].z;
		it->u[i] = xcenter + xscale * zi * ends[i].x;
		it->v[i] = ycenter - yscale * zi * ends[i].y;
		it->lz[i] = zi;
		memcpy (it->lrgba[i], ends[i].rgba, sizeof(it->lrgba[i]));
	}
	it->miny = (int)floorf (fminf (it->v[0], it->v[1]) + 0.5f);
	it->maxy = (int)floorf (fmaxf (it->v[0], it->v[1]) + 0.5f) + 1;
	it->miny = it->miny < r_refdef.vrect.y ? r_refdef.vrect.y : it->miny;
	it->maxy = it->maxy > r_refdef.vrect.y + r_refdef.vrect.height ? r_refdef.vrect.y + r_refdef.vrect.height : it->maxy;
	if (it->miny >= it->maxy)
		return false;
	it->fog = R_PartFog ((it->lz[0] + it->lz[1]) * 0.5f);
	it->varying = true;
	it->kind = ITEM_LINE;
	return true;
}

// the size of a sprite or a fan with distance, as QuakeSpasm-Spiked has it
static float R_PartScale (const r_partbatch_t *b, const r_part_t *p, float base, float slope)
{
	vec3_t	d;
	float	s;

	VectorSubtract (p->org, r_origin, d);
	s = DotProduct (d, vpn) * p->scale * b->invscalefactor + p->scale * (b->scalefactor * 250);
	return s < 20 ? base : base + s * slope;
}

static bool R_PartNormalize (vec3_t v)
{
	float	len = sqrtf (DotProduct (v, v));

	if (len < 1e-6f)
		return false;
	VectorScale (v, 1 / len, v);
	return true;
}

/*
=================
R_PartSetup

The item of a batch's particle i
=================
*/
static void R_PartSetup (const r_partbatch_t *b, int i, partitem_t *it)
{
	const r_part_t		*p, *q;
	const r_partvert_t	*dv;
	partvert_t			c[4];
	vec3_t				pright, pup, o2, v, cr, dir, corner, sdir = {1, 0, 0}, tdir = {0, 1, 0};
	vec3_t				org, vel, qorg, qvel;
	float				scale, x, y, length, half;
	int					k;

	it->kind = ITEM_NONE;
	switch (b->type)
	{
	case RPT_SPRITE:
		p = &r_partscene->parts[b->first + i];
		VectorCopy (p->org, org);
		VectorCopy (p->vel, vel);
		scale = b->scalefactor == 1 ? p->scale * 0.25f : R_PartScale (b, p, 0.25f, 0.001f);
		VectorScale (vup, 1.5f, pup);
		VectorScale (vright, 1.5f, pright);
		if (p->angle)
		{
			x = sinf (p->angle) * scale;
			y = cosf (p->angle) * scale;
			for (k = 0 ; k < 3 ; k++)
			{
				corner[k] = org[k] - x * pright[k] - y * pup[k];
				o2[k] = org[k] - y * pright[k] + x * pup[k];
			}
			R_PartCorner (&c[0], corner, p->st[0], p->st[1], p->rgba);
			R_PartCorner (&c[1], o2, p->st[0], p->st[3], p->rgba);
			for (k = 0 ; k < 3 ; k++)
			{
				corner[k] = org[k] + x * pright[k] + y * pup[k];
				o2[k] = org[k] + y * pright[k] - x * pup[k];
			}
			R_PartCorner (&c[2], corner, p->st[2], p->st[3], p->rgba);
			R_PartCorner (&c[3], o2, p->st[2], p->st[1], p->rgba);
		}
		else
		{
			VectorMA (org, -scale, pup, corner);
			R_PartCorner (&c[0], corner, p->st[0], p->st[1], p->rgba);
			VectorMA (org, -scale, pright, corner);
			R_PartCorner (&c[1], corner, p->st[0], p->st[3], p->rgba);
			VectorMA (org, scale, pup, corner);
			R_PartCorner (&c[2], corner, p->st[2], p->st[3], p->rgba);
			VectorMA (org, scale, pright, corner);
			R_PartCorner (&c[3], corner, p->st[2], p->st[1], p->rgba);
		}
		memcpy (it->rgba, p->rgba, sizeof(it->rgba));
		R_PartPolygon (it, c, 4);
		break;

	case RPT_UDECAL:
		p = &r_partscene->parts[b->first + i];
		VectorCopy (p->org, org);
		VectorCopy (p->vel, vel);
		if (p->angle)
		{
			x = sinf (p->angle) * p->scale;
			y = cosf (p->angle) * p->scale;
			for (k = 0 ; k < 3 ; k++)
			{
				corner[k] = org[k] - x * sdir[k] - y * tdir[k];
				o2[k] = org[k] - y * sdir[k] + x * tdir[k];
			}
			R_PartCorner (&c[0], corner, p->st[0], p->st[1], p->rgba);
			R_PartCorner (&c[1], o2, p->st[0], p->st[3], p->rgba);
			for (k = 0 ; k < 3 ; k++)
			{
				corner[k] = org[k] + x * sdir[k] + y * tdir[k];
				o2[k] = org[k] + y * sdir[k] - x * tdir[k];
			}
			R_PartCorner (&c[2], corner, p->st[2], p->st[3], p->rgba);
			R_PartCorner (&c[3], o2, p->st[2], p->st[1], p->rgba);
		}
		else
		{
			VectorMA (org, -p->scale, tdir, corner);
			R_PartCorner (&c[0], corner, p->st[0], p->st[1], p->rgba);
			VectorMA (org, -p->scale, sdir, corner);
			R_PartCorner (&c[1], corner, p->st[0], p->st[3], p->rgba);
			VectorMA (org, p->scale, tdir, corner);
			R_PartCorner (&c[2], corner, p->st[2], p->st[3], p->rgba);
			VectorMA (org, p->scale, sdir, corner);
			R_PartCorner (&c[3], corner, p->st[2], p->st[1], p->rgba);
		}
		memcpy (it->rgba, p->rgba, sizeof(it->rgba));
		R_PartPolygon (it, c, 4);
		break;

	case RPT_TSPARK:
		p = &r_partscene->parts[b->first + i];
		VectorCopy (p->org, org);
		VectorCopy (p->vel, vel);
		half = p->scale * 0.5f;
		VectorCopy (vel, dir);
		length = sqrtf (DotProduct (dir, dir));
		if (length < 1e-6f)
			break;
		VectorScale (dir, 1 / length, dir);
		length = b->stretch < 0 ? -b->stretch : length * b->stretch;
		if (length < half * b->minstretch)
			length = half * b->minstretch;
		VectorMA (org, -length, dir, o2);
		VectorSubtract (r_origin, o2, v);
		CrossProduct (v, vel, cr);
		if (!R_PartNormalize (cr))
			break;
		VectorMA (o2, -half, cr, corner);
		R_PartCorner (&c[0], corner, p->st[0], p->st[1], p->rgba);
		VectorMA (o2, half, cr, corner);
		R_PartCorner (&c[1], corner, p->st[0], p->st[3], p->rgba);
		VectorMA (org, length, dir, o2);
		VectorSubtract (r_origin, o2, v);
		CrossProduct (v, vel, cr);
		if (!R_PartNormalize (cr))
			break;
		VectorMA (o2, half, cr, corner);
		R_PartCorner (&c[2], corner, p->st[2], p->st[3], p->rgba);
		VectorMA (o2, -half, cr, corner);
		R_PartCorner (&c[3], corner, p->st[2], p->st[1], p->rgba);
		memcpy (it->rgba, p->rgba, sizeof(it->rgba));
		R_PartPolygon (it, c, 4);
		break;

	case RPT_FAN:
		p = &r_partscene->parts[b->first + i];
		VectorCopy (p->org, org);
		VectorCopy (p->vel, vel);
		scale = R_PartScale (b, p, 0.05f, 0.0001f);
		VectorMA (org, -scale, vel, o2);
		VectorSubtract (r_origin, o2, v);
		CrossProduct (v, vel, cr);
		if (!R_PartNormalize (cr))
			break;
		R_PartCorner (&c[0], org, p->st[0], p->st[1], p->rgba);
		VectorMA (o2, -p->scale, cr, corner);
		R_PartCorner (&c[1], corner, p->st[0], p->st[3], p->rgba);
		VectorMA (o2, p->scale, cr, corner);
		R_PartCorner (&c[2], corner, p->st[2], p->st[1], p->rgba);
		memcpy (it->rgba, p->rgba, sizeof(it->rgba));
		R_PartPolygon (it, c, 3);
		break;

	case RPT_SPARK:
		{
			float	tail[4];

			p = &r_partscene->parts[b->first + i];
			VectorCopy (p->org, org);
			VectorCopy (p->vel, vel);
			memcpy (tail, p->rgba, sizeof(tail));
			tail[3] = 0;
			R_PartCorner (&c[0], org, 0, 0, p->rgba);
			VectorMA (org, -0.1f, vel, corner);
			R_PartCorner (&c[1], corner, 0, 0, tail);
			memcpy (it->rgba, p->rgba, sizeof(it->rgba));
			R_PartLine (it, &c[0], &c[1]);
		}
		break;

	case RPT_BEAM:
		q = &r_partscene->parts[b->first + i * 2];
		p = q + 1;
		VectorCopy (p->org, org);
		VectorCopy (p->vel, vel);
		VectorCopy (q->org, qorg);
		VectorCopy (q->vel, qvel);
		VectorSubtract (r_origin, qorg, v);
		R_PartNormalize (v);
		CrossProduct (qvel, v, cr);
		if (!R_PartNormalize (cr))
			break;
		VectorMA (qorg, -q->scale, cr, corner);
		R_PartCorner (&c[0], corner, q->st[0], q->st[1], q->rgba);
		VectorMA (qorg, q->scale, cr, corner);
		R_PartCorner (&c[1], corner, q->st[0], q->st[3], q->rgba);
		VectorSubtract (r_origin, org, v);
		R_PartNormalize (v);
		CrossProduct (vel, v, cr);
		if (!R_PartNormalize (cr))
			break;
		VectorMA (org, p->scale, cr, corner);
		R_PartCorner (&c[2], corner, p->st[0], p->st[3], p->rgba);
		VectorMA (org, -p->scale, cr, corner);
		R_PartCorner (&c[3], corner, p->st[0], p->st[1], p->rgba);
		it->varying = true;
		R_PartPolygon (it, c, 4);
		break;

	case RPT_DECAL:
		dv = &r_partscene->verts[b->first + i * 3];
		for (k = 0 ; k < 3 ; k++)
			R_PartCorner (&c[k], dv[k].xyz, dv[k].st[0], dv[k].st[1], dv[k].rgba);
		it->varying = true;
		it->bias = 1 + PART_DECALBIAS;
		R_PartPolygon (it, c, 3);
		break;
	}
}

// the items of a batch
static int R_PartBatchItems (const r_partbatch_t *b)
{
	return b->type == RPT_BEAM ? b->count / 2 : b->type == RPT_DECAL ? b->count / 3 : b->count;
}

/*
=================
R_PartSetupJob

The items of PART_CHUNK particles from the chunk's first, the batches' looks
in each
=================
*/
static void R_PartSetupJob (void *ctx, int chunk)
{
	const r_partbatch_t	*b;
	partitem_t			*it;
	int					first = chunk * PART_CHUNK, i, n, lo, hi, mid, bi;

	(void)ctx;
	// the batch the chunk starts in
	lo = 0;
	hi = r_partscene->numbatches - 1;
	while (lo < hi)
	{
		mid = (lo + hi + 1) / 2;
		if (r_partfirst[mid] <= first)
			lo = mid;
		else
			hi = mid - 1;
	}
	n = r_partfirst[r_partscene->numbatches];
	for (bi = lo, i = first ; i < first + PART_CHUNK && i < n ; i++)
	{
		while (i >= r_partfirst[bi + 1])
			bi++;
		b = &r_partscene->batches[bi];
		it = &r_partitems[i];
		memset (it, 0, offsetof(partitem_t, u));
		it->bias = 1;
		it->image = b->type == RPT_SPARK ? NULL : R_PartImage (b->image);
		switch (b->blend)
		{
		case RPB_BLEND:		it->shade = SHADE_LIGHT; it->weightalpha = it->covers = true; break;
		case RPB_ADDA:		it->shade = SHADE_LIGHT; it->weightalpha = true; break;
		case RPB_PREMUL:	it->shade = SHADE_LIGHT; it->weightalpha = b->premul != 0; it->covers = b->premul != 2; break;
		case RPB_BLENDCOLOR:	it->shade = SHADE_BLENDCOLOR; break;
		case RPB_ADDC:		it->shade = SHADE_ADDC; break;
		case RPB_SUBTRACT:	it->shade = SHADE_SUBTRACT; break;
		case RPB_INVMODA:	it->shade = SHADE_INVMODA; break;
		case RPB_INVMODC:	it->shade = SHADE_INVMODC; it->weightalpha = b->premul != 0; break;
		}
		R_PartSetup (b, i - r_partfirst[bi], it);
	}
}

/*
==============================================================================

FILLING: the items in bands of rows

==============================================================================
*/

// what a texel of color rgba (sRGB, 0..1) adds (s, light as a channel to the
// fourth) and multiplies (m), through the fog's leaving fog of it
static inline void R_PartShade (const partitem_t *it, const byte *texel, const float *rgba, const float *light,
	float *s, float *m)
{
	float	ta = texel[3] * (1.0f / 255), ca = rgba[3] < 1 ? rgba[3] : 1, k = it->fog, tc, lin, cover, w;
	int		c;

	ca = ca > 0 ? ca : 0;
	switch (it->shade)
	{
	case SHADE_LIGHT:
		w = ta * (it->weightalpha ? ca : 1) * k;
		cover = it->covers ? ta * ca : 0;
		for (c = 0 ; c < 3 ; c++)
		{
			lin = it->varying ? R_PartLinear (rgba[c]) : it->lin[c];
			s[c] = light[texel[c]] * lin * w + d_fog.color[c] * (1 - k) * cover;
			m[c] = 1 - cover;
		}
		break;
	case SHADE_INVMODA:
		lin = R_PartLinear (1 - ta * ca);
		for (c = 0 ; c < 3 ; c++)
		{
			s[c] = 0;
			m[c] = 1 - (1 - lin) * k;
		}
		break;
	case SHADE_INVMODC:
		w = it->weightalpha ? ta * ca : 1;
		for (c = 0 ; c < 3 ; c++)
		{
			lin = R_PartLinear (1 - texel[c] * (1.0f / 255) * rgba[c] * w);
			s[c] = 0;
			m[c] = 1 - (1 - lin) * k;
		}
		break;
	case SHADE_ADDC:
		for (c = 0 ; c < 3 ; c++)
		{
			tc = texel[c] * (1.0f / 255) * rgba[c];
			lin = R_PartLinear (tc);
			s[c] = lin * lin * k * PART_WHITE;
			m[c] = 1;
		}
		break;
	case SHADE_BLENDCOLOR:
	case SHADE_SUBTRACT:
		for (c = 0 ; c < 3 ; c++)
		{
			tc = texel[c] * (1.0f / 255) * rgba[c];
			tc = tc < 1 ? tc : 1;
			s[c] = R_PartLinear (tc) * (it->shade == SHADE_BLENDCOLOR ? tc : ta * ca) * k * PART_WHITE
				+ d_fog.color[c] * (1 - k) * tc;
			m[c] = 1 - tc;
		}
		break;
	}
}

static inline const byte *R_PartTexel (const partimage_t *img, int level, float s, float t)
{
	int		w = img->lw[level], h = img->lh[level];
	int		x = (int)floorf (s * (float)w) % w, y = (int)floorf (t * (float)h) % h;

	x += x < 0 ? w : 0;
	y += y < 0 ? h : 0;
	return img->levels[level] + (y * w + x) * 4;
}

/*
=================
R_PartRow

A polygon item's row y, blended in from its own S and M rows
=================
*/
static void R_PartRow (const partitem_t *it, int y, float *rows)
{
	static const byte	white[4] = {255, 255, 255, 255};
	const float			*light = R_SrgbLightTable ();
	const float			*src[3] = {rows, rows + r_partrowsize, rows + 2 * r_partrowsize};
	const float			*mul[3] = {rows + 3 * r_partrowsize, rows + 4 * r_partrowsize, rows + 5 * r_partrowsize};
	float				*s[3] = {rows, rows + r_partrowsize, rows + 2 * r_partrowsize};
	float				*m[3] = {rows + 3 * r_partrowsize, rows + 4 * r_partrowsize, rows + 5 * r_partrowsize};
	float				xl = 1e30f, xr = -1e30f, x, fy = (float)y, zi, inv, sz, tz, rgba[4], out[3], mo[3];
	const byte			*texel;
	int					i, j, k, x0, x1, count, c;
	bool				affine = fabsf (it->plane[0][0]) + fabsf (it->plane[0][1]) <= 1e-9f * fabsf (it->plane[0][2]);

	for (i = 0 ; i < it->nv ; i++)
	{
		j = (i + 1) % it->nv;
		if ((it->v[i] <= fy && fy < it->v[j]) || (it->v[j] <= fy && fy < it->v[i]))
		{
			x = it->u[i] + (fy - it->v[i]) * (it->u[j] - it->u[i]) / (it->v[j] - it->v[i]);
			xl = fminf (xl, x);
			xr = fmaxf (xr, x);
		}
	}
	if (xl > xr)
		return;
	x0 = (int)ceilf (fmaxf (xl, (float)r_refdef.vrect.x));
	x1 = (int)ceilf (fminf (xr, (float)(r_refdef.vrect.x + r_refdef.vrect.width)));
	count = x1 - x0;
	if (count <= 0)
		return;

	zi = it->plane[0][0] * (float)x0 + it->plane[0][1] * fy + it->plane[0][2];
	sz = it->plane[1][0] * (float)x0 + it->plane[1][1] * fy + it->plane[1][2];
	tz = it->plane[2][0] * (float)x0 + it->plane[2][1] * fy + it->plane[2][2];
	memcpy (rgba, it->rgba, sizeof(rgba));
	inv = 1 / zi;
	for (i = 0 ; i < count ; i++)
	{
		if (!affine)
			inv = 1 / (zi + (float)i * it->plane[0][0]);
		texel = it->image ? R_PartTexel (it->image, it->level, (sz + (float)i * it->plane[1][0]) * inv,
			(tz + (float)i * it->plane[2][0]) * inv) : white;
		if (it->varying)
			for (k = 0 ; k < 4 ; k++)
				rgba[k] = (it->plane[3+k][0] * (float)(x0 + i) + it->plane[3+k][1] * fy + it->plane[3+k][2]) * inv;
		R_PartShade (it, texel, rgba, light, out, mo);
		for (c = 0 ; c < 3 ; c++)
		{
			s[c][i] = out[c];
			m[c][i] = mo[c];
		}
	}
	simd_partspan (d_viewbuffer + d_scantable[y] + x0, d_pzbuffer + d_zwidth * y + x0, zi * it->bias,
		it->plane[0][0] * it->bias, src, mul, count);
}

// a pixel's light added and multiplied, as simd_partspan does it
static inline unsigned R_PartChannel (unsigned d, float s, float m)
{
	float		fd = (float)d, x;
	unsigned	c;

	fd *= fd;
	fd *= fd;
	x = s + fd * m;
	c = (unsigned)(sqrtf (sqrtf (x > 0 ? x : 0)) + 0.5f);
	return c < 1023 ? c : 1023;
}

/*
=================
R_PartLineRows

A line item's pixels in rows y0 to y1, one a step along its longer axis
=================
*/
static void R_PartLineRows (const partitem_t *it, int y0, int y1)
{
	static const byte	white[4] = {255, 255, 255, 255};
	const float			*light = R_SrgbLightTable ();
	float				du = it->u[1] - it->u[0], dv = it->v[1] - it->v[0], f, zi, rgba[4], s[3], m[3];
	pixel_t				*pdest, d;
	int					steps, i, k, x, y;

	steps = (int)ceilf (fmaxf (fabsf (du), fabsf (dv)));
	steps = steps < 1 ? 1 : steps > PART_MAXLINE ? PART_MAXLINE : steps;
	for (i = 0 ; i <= steps ; i++)
	{
		f = (float)i / (float)steps;
		x = (int)floorf (it->u[0] + f * du + 0.5f);
		y = (int)floorf (it->v[0] + f * dv + 0.5f);
		if (y < y0 || y >= y1 || x < r_refdef.vrect.x || x >= r_refdef.vrect.x + r_refdef.vrect.width)
			continue;
		zi = it->lz[0] + f * (it->lz[1] - it->lz[0]);
		if (d_pzbuffer[d_zwidth * y + x] > zi)
			continue;
		for (k = 0 ; k < 4 ; k++)
			rgba[k] = (it->lrgba[0][k] * it->lz[0] + f * (it->lrgba[1][k] * it->lz[1] - it->lrgba[0][k] * it->lz[0])) / zi;
		R_PartShade (it, white, rgba, light, s, m);
		pdest = d_viewbuffer + d_scantable[y] + x;
		d = *pdest;
		*pdest = R_PartChannel (d & 1023, s[0], m[0]) | (R_PartChannel ((d >> 10) & 1023, s[1], m[1]) << 10)
			| (R_PartChannel ((d >> 20) & 1023, s[2], m[2]) << 20);
	}
}

// a band of rows: its items in order
static void R_PartBandJob (void *ctx, int band)
{
	const partitem_t	*it;
	float				*rows = r_partrows + (size_t)band * 6 * (size_t)r_partrowsize;
	int					y0 = r_refdef.vrect.y + band * PART_BAND, y1, i, y, from, to;

	(void)ctx;
	y1 = y0 + PART_BAND;
	if (y1 > r_refdef.vrect.y + r_refdef.vrect.height)
		y1 = r_refdef.vrect.y + r_refdef.vrect.height;
	for (i = r_partbandstart[band] ; i < r_partbandstart[band + 1] ; i++)
	{
		it = &r_partitems[r_partbands[i]];
		if (it->kind == ITEM_LINE)
		{
			R_PartLineRows (it, y0, y1);
			continue;
		}
		from = it->miny > y0 ? it->miny : y0;
		to = it->maxy < y1 ? it->maxy : y1;
		for (y = from ; y < to ; y++)
			R_PartRow (it, y, rows);
	}
}

/*
=================
R_DrawPartScene

r_scene.particles into the view
=================
*/
void R_DrawPartScene (void)
{
	const r_partscene_t	*ps = r_scene.particles;
	int		i, b, n, numbands, total, first, last;

	if (!ps || !ps->numbatches)
		return;
	r_partscene = ps;

	// each batch's first item
	if (ps->numbatches + 1 > r_maxbatches)
	{
		r_maxbatches = ps->numbatches + 1 + 64;
		r_partfirst = Mem_Realloc (r_partfirst, (size_t)r_maxbatches * sizeof(*r_partfirst));
	}
	for (b = 0, n = 0 ; b < ps->numbatches ; b++)
	{
		r_partfirst[b] = n;
		n += R_PartBatchItems (&ps->batches[b]);
	}
	r_partfirst[ps->numbatches] = n;
	if (!n)
		return;
	if (n > r_maxpartitems)
	{
		r_maxpartitems = n + n / 2;
		r_partitems = Mem_Realloc (r_partitems, (size_t)r_maxpartitems * sizeof(*r_partitems));
	}
	Sys_Parallel ((n + PART_CHUNK - 1) / PART_CHUNK, R_PartSetupJob, NULL);

	// the items in the bands of rows they cover, in their order
	numbands = (r_refdef.vrect.height + PART_BAND - 1) / PART_BAND;
	if (numbands + 1 > r_maxbands)
	{
		r_maxbands = numbands + 1;
		r_partbandstart = Mem_Realloc (r_partbandstart, (size_t)r_maxbands * sizeof(*r_partbandstart));
	}
	memset (r_partbandstart, 0, (size_t)(numbands + 1) * sizeof(*r_partbandstart));
	for (i = 0, total = 0 ; i < n ; i++)
	{
		if (r_partitems[i].kind == ITEM_NONE)
			continue;
		first = (r_partitems[i].miny - r_refdef.vrect.y) / PART_BAND;
		last = (r_partitems[i].maxy - 1 - r_refdef.vrect.y) / PART_BAND;
		for (b = first ; b <= last ; b++)
			r_partbandstart[b + 1]++;
		total += last - first + 1;
	}
	if (!total)
		return;
	for (b = 0 ; b < numbands ; b++)
		r_partbandstart[b + 1] += r_partbandstart[b];
	if (total > r_maxbandlist)
	{
		r_maxbandlist = total + total / 2;
		r_partbands = Mem_Realloc (r_partbands, (size_t)r_maxbandlist * sizeof(*r_partbands));
	}
	{
		int	*fill = Mem_Alloc ((size_t)numbands * sizeof(*fill));

		memcpy (fill, r_partbandstart, (size_t)numbands * sizeof(*fill));
		for (i = 0 ; i < n ; i++)
		{
			if (r_partitems[i].kind == ITEM_NONE)
				continue;
			first = (r_partitems[i].miny - r_refdef.vrect.y) / PART_BAND;
			last = (r_partitems[i].maxy - 1 - r_refdef.vrect.y) / PART_BAND;
			for (b = first ; b <= last ; b++)
				r_partbands[fill[b]++] = i;
		}
		Mem_Free (fill);
	}

	// each band's rows of light and multipliers
	if (r_refdef.vrect.width > r_partrowsize || numbands > r_partrowbands)
	{
		r_partrowsize = r_refdef.vrect.width > r_partrowsize ? r_refdef.vrect.width : r_partrowsize;
		r_partrowbands = numbands > r_partrowbands ? numbands : r_partrowbands;
		r_partrows = Mem_Realloc (r_partrows, (size_t)r_partrowbands * 6 * (size_t)r_partrowsize * sizeof(float));
	}
	Sys_Parallel (numbands, R_PartBandJob, NULL);
}
