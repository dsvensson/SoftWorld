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
// r_alias.c: routines for setting up to draw alias models

#include "r_local.h"
#include "r_local.h"
#include "d_local.h"	// FIXME: shouldn't be needed (is needed for patch
						// right now, but that should move)

#define LIGHT_MIN	5		// lowest light value we'll allow, to avoid the
							//  need for inner-loop light clamping

affinetridesc_t	r_affinetridesc;


static trivertx_t		*r_apverts;		// the frame's pose
static trivertx_t		*r_aoldverts;	// the pose turned from (r_lerpframes), NULL for none
static float			r_alerp;		// how far from it to the frame's, 0 .. 1
static bool				r_amuzzlehack;	// a view model's muzzle flash goes at once

// TODO: these probably will go away with optimized rasterization
static mdl_t				*pmdl;
static vec3_t				r_plightvec;
static int					r_ambientlight;
static float				r_shadelight;
static aliashdr_t			*paliashdr;
finalvert_t			*pfinalverts;
auxvert_t			*pauxverts;
static float		ziscale;
static float		r_aliaszmul;		// 3 for the view model

// 1/z in 31 bit fixed point; nearer than that stays the nearest
static int R_AliasFixedZi (float zi)
{
	return zi < 2147483520.0f ? (int)zi : 2147483520;
}
static model_t		*pmodel;

static vec3_t		alias_forward, alias_right, alias_up;

static maliasskindesc_t	*pskindesc;

int				r_amodels_drawn;
static int				a_skinwidth;
static int				r_anumverts;

static float	aliastransform[3][4];

typedef struct {
	int	index0;
	int	index1;
} aedge_t;

static aedge_t	aedges[12] = {
{0, 1}, {1, 2}, {2, 3}, {3, 0},
{4, 5}, {5, 6}, {6, 7}, {7, 4},
{0, 5}, {1, 4}, {2, 7}, {3, 6}
};

#define NUMVERTEXNORMALS	162

static float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.inc"
};

static void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv,
	stvert_t *pstverts);
static void R_AliasSetUpTransform (int trivial_accept);
static void R_AliasTransformVector (vec3_t in, vec3_t out);
static void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	int vert, stvert_t *pstverts);
void R_AliasProjectFinalVert (finalvert_t *fv, auxvert_t *av);

/*
================
R_AliasTurning

Whether currententity is drawn between its frame and the one it turns from
(r_lerpframes), its model's header pahdr
================
*/
static bool R_AliasTurning (const aliashdr_t *pahdr)
{
	const mdl_t	*m = (const mdl_t *)((const byte *)pahdr + pahdr->model);

	return r_lerpframes.value && currententity->backlerp > 0
		&& currententity->oldframe != currententity->frame
		&& currententity->oldframe >= 0 && currententity->oldframe < m->numframes;
}


/*
================
R_AliasCheckBBox
================
*/
bool R_AliasCheckBBox (void)
{
	int					i, flags, frame, numv;
	aliashdr_t			*pahdr;
	float				zi, basepts[8][3], v0, v1, frac, mins[3], maxs[3];
	finalvert_t			*pv0, *pv1, viewpts[16];
	auxvert_t			*pa0, *pa1, viewaux[16];
	maliasframedesc_t	*pframedesc;
	bool			zclipped, zfullyclipped;
	unsigned			anyclip, allclip;
	int					minz;
	
// expand, rotate, and translate points into worldspace

	currententity->trivial_accept = 0;
	pmodel = currententity->model;
	pahdr = Mod_Extradata (pmodel);
	pmdl = (mdl_t *)((byte *)pahdr + pahdr->model);

	R_AliasSetUpTransform (0);

// construct the base bounding box for this frame
	frame = currententity->frame;
// TODO: don't repeat this check when drawing?
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("No such frame %d %s\n", frame,
				pmodel->name);
		frame = 0;
	}

	pframedesc = &pahdr->frames[frame];
	for (i=0 ; i<3 ; i++)
	{
		mins[i] = (float)pframedesc->bboxmin.v[i];
		maxs[i] = (float)pframedesc->bboxmax.v[i];
	}

// turning from another frame, the vertices are within the two frames' boxes;
// a trivially accepted model isn't clipped, so they must be
	if (R_AliasTurning (pahdr))
	{
		pframedesc = &pahdr->frames[currententity->oldframe];
		for (i=0 ; i<3 ; i++)
		{
			if (mins[i] > pframedesc->bboxmin.v[i])
				mins[i] = (float)pframedesc->bboxmin.v[i];
			if (maxs[i] < pframedesc->bboxmax.v[i])
				maxs[i] = (float)pframedesc->bboxmax.v[i];
		}
	}

// x worldspace coordinates
	basepts[0][0] = basepts[1][0] = basepts[2][0] = basepts[3][0] = mins[0];
	basepts[4][0] = basepts[5][0] = basepts[6][0] = basepts[7][0] = maxs[0];

// y worldspace coordinates
	basepts[0][1] = basepts[3][1] = basepts[5][1] = basepts[6][1] = mins[1];
	basepts[1][1] = basepts[2][1] = basepts[4][1] = basepts[7][1] = maxs[1];

// z worldspace coordinates
	basepts[0][2] = basepts[1][2] = basepts[4][2] = basepts[5][2] = mins[2];
	basepts[2][2] = basepts[3][2] = basepts[6][2] = basepts[7][2] = maxs[2];

	zclipped = false;
	zfullyclipped = true;

	minz = 9999;
	for (i=0; i<8 ; i++)
	{
		R_AliasTransformVector  (&basepts[i][0], &viewaux[i].fv[0]);

		if (viewaux[i].fv[2] < ALIAS_Z_CLIP_PLANE)
		{
		// we must clip points that are closer than the near clip plane
			viewpts[i].flags = ALIAS_Z_CLIP;
			zclipped = true;
		}
		else
		{
			if (viewaux[i].fv[2] < minz)
				minz = (int)viewaux[i].fv[2];
			viewpts[i].flags = 0;
			zfullyclipped = false;
		}
	}

	
	if (zfullyclipped)
	{
		return false;	// everything was near-z-clipped
	}

	numv = 8;

	if (zclipped)
	{
	// organize points by edges, use edges to get new points (possible trivial
	// reject)
		for (i=0 ; i<12 ; i++)
		{
		// edge endpoints
			pv0 = &viewpts[aedges[i].index0];
			pv1 = &viewpts[aedges[i].index1];
			pa0 = &viewaux[aedges[i].index0];
			pa1 = &viewaux[aedges[i].index1];

		// if one end is clipped and the other isn't, make a new point
			if (pv0->flags ^ pv1->flags)
			{
				frac = (ALIAS_Z_CLIP_PLANE - pa0->fv[2]) /
					   (pa1->fv[2] - pa0->fv[2]);
				viewaux[numv].fv[0] = pa0->fv[0] +
						(pa1->fv[0] - pa0->fv[0]) * frac;
				viewaux[numv].fv[1] = pa0->fv[1] +
						(pa1->fv[1] - pa0->fv[1]) * frac;
				viewaux[numv].fv[2] = ALIAS_Z_CLIP_PLANE;
				viewpts[numv].flags = 0;
				numv++;
			}
		}
	}

// project the vertices that remain after clipping
	anyclip = 0;
	allclip = ALIAS_XY_CLIP_MASK;

// TODO: probably should do this loop in ASM, especially if we use floats
	for (i=0 ; i<numv ; i++)
	{
	// we don't need to bother with vertices that were z-clipped
		if (viewpts[i].flags & ALIAS_Z_CLIP)
			continue;

		zi = 1.0f / viewaux[i].fv[2];

	// FIXME: do with chop mode in ASM, or convert to float
		v0 = (viewaux[i].fv[0] * xscale * zi) + xcenter;
		v1 = (viewaux[i].fv[1] * yscale * zi) + ycenter;

		flags = 0;

		if (v0 < r_refdef.fvrectx)
			flags |= ALIAS_LEFT_CLIP;
		if (v1 < r_refdef.fvrecty)
			flags |= ALIAS_TOP_CLIP;
		if (v0 > r_refdef.fvrectright)
			flags |= ALIAS_RIGHT_CLIP;
		if (v1 > r_refdef.fvrectbottom)
			flags |= ALIAS_BOTTOM_CLIP;

		anyclip |= flags;
		allclip &= flags;
	}

	if (allclip)
		return false;	// trivial reject off one side

	currententity->trivial_accept = !anyclip & !zclipped;

	if (currententity->trivial_accept)
	{
		if (minz > (r_aliastransition + (pmdl->size * r_resfudge)))
		{
			currententity->trivial_accept |= 2;
		}
	}

	return true;
}


/*
================
R_AliasTransformVector
================
*/
static void R_AliasTransformVector (vec3_t in, vec3_t out)
{
	out[0] = DotProduct(in, aliastransform[0]) + aliastransform[0][3];
	out[1] = DotProduct(in, aliastransform[1]) + aliastransform[1][3];
	out[2] = DotProduct(in, aliastransform[2]) + aliastransform[2][3];
}


/*
================
R_AliasPreparePoints

General clipped case
================
*/
static void R_AliasPreparePoints (void)
{
	int			i;
	stvert_t	*pstverts;
	finalvert_t	*fv;
	auxvert_t	*av;
	mtriangle_t	*ptri;
	finalvert_t	*pfv[3];

	pstverts = (stvert_t *)((byte *)paliashdr + paliashdr->stverts);
	r_anumverts = pmdl->numverts;
 	fv = pfinalverts;
	av = pauxverts;

	for (i=0 ; i<r_anumverts ; i++, fv++, av++, pstverts++)
	{
		R_AliasTransformFinalVert (fv, av, i, pstverts);
		if (av->fv[2] < ALIAS_Z_CLIP_PLANE)
			fv->flags |= ALIAS_Z_CLIP;
		else
		{
			 R_AliasProjectFinalVert (fv, av);

			if (fv->v[0] < r_refdef.aliasvrect.x)
				fv->flags |= ALIAS_LEFT_CLIP;
			if (fv->v[1] < r_refdef.aliasvrect.y)
				fv->flags |= ALIAS_TOP_CLIP;
			if (fv->v[0] > r_refdef.aliasvrectright)
				fv->flags |= ALIAS_RIGHT_CLIP;
			if (fv->v[1] > r_refdef.aliasvrectbottom)
				fv->flags |= ALIAS_BOTTOM_CLIP;	
		}
	}

//
// clip and draw all triangles
//
	r_affinetridesc.numtriangles = 1;

	ptri = (mtriangle_t *)((byte *)paliashdr + paliashdr->triangles);
	for (i=0 ; i<pmdl->numtris ; i++, ptri++)
	{
		pfv[0] = &pfinalverts[ptri->vertindex[0]];
		pfv[1] = &pfinalverts[ptri->vertindex[1]];
		pfv[2] = &pfinalverts[ptri->vertindex[2]];

		if ( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags & (ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) )
			continue;		// completely clipped
		
		if ( ! ( (pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) &
			(ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) ) )
		{	// totally unclipped
			r_affinetridesc.pfinalverts = pfinalverts;
			r_affinetridesc.ptriangles = ptri;
			D_PolysetDraw ();
		}
		else		
		{	// partially clipped
			R_AliasClipTriangle (ptri);
		}
	}
}


/*
================
R_AliasSetUpTransform
================
*/
static void R_AliasSetUpTransform (int trivial_accept)
{
	int				i;
	float			rotationmatrix[3][4], t2matrix[3][4];
	static float	tmatrix[3][4];
	static float	viewmatrix[3][4];
	vec3_t			angles;

// TODO: should really be stored with the entity instead of being reconstructed
// TODO: should use a look-up table
// TODO: could cache lazily, stored in the entity

	angles[ROLL] = currententity->angles[ROLL];
	angles[PITCH] = -currententity->angles[PITCH];
	angles[YAW] = currententity->angles[YAW];
	AngleVectors (angles, alias_forward, alias_right, alias_up);

	tmatrix[0][0] = pmdl->scale[0];
	tmatrix[1][1] = pmdl->scale[1];
	tmatrix[2][2] = pmdl->scale[2];

	tmatrix[0][3] = pmdl->scale_origin[0];
	tmatrix[1][3] = pmdl->scale_origin[1];
	tmatrix[2][3] = pmdl->scale_origin[2];

// TODO: can do this with simple matrix rearrangement

	for (i=0 ; i<3 ; i++)
	{
		t2matrix[i][0] = alias_forward[i];
		t2matrix[i][1] = -alias_right[i];
		t2matrix[i][2] = alias_up[i];
	}

	t2matrix[0][3] = -modelorg[0];
	t2matrix[1][3] = -modelorg[1];
	t2matrix[2][3] = -modelorg[2];

// FIXME: can do more efficiently than full concatenation
	R_ConcatTransforms (t2matrix, tmatrix, rotationmatrix);

// TODO: should be global, set when vright, etc., set
	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup, viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn, viewmatrix[2]);

//	viewmatrix[0][3] = 0;
//	viewmatrix[1][3] = 0;
//	viewmatrix[2][3] = 0;

	R_ConcatTransforms (viewmatrix, rotationmatrix, aliastransform);

// do the scaling up of x and y to screen coordinates as part of the transform
// for the unclipped case (it would mess up clipping in the clipped case).
// Also scale down z, so 1/z is scaled 31 bits for free, and scale down x and y
// correspondingly so the projected x and y come out right
// FIXME: make this work for clipped case too?
	if (trivial_accept)
	{
		for (i=0 ; i<4 ; i++)
		{
			aliastransform[0][i] = (float)(aliastransform[0][i] *
					(aliasxscale * (1.0 / ((float)0x8000 * 0x10000))));
			aliastransform[1][i] = (float)(aliastransform[1][i] *
					(aliasyscale * (1.0 / ((float)0x8000 * 0x10000))));
			aliastransform[2][i] *= 1.0 / ((float)0x8000 * 0x10000);

		}
	}
}



// a vertex's light, (255 - level) << 6 with a level of 8192 being 1.0: with
// RGB light the level's fourth root, which is what multiplies a pixel (vid.h)
static int R_AliasVertexLight (int light)
{
	static short	root[(255 << 6) + 1];		// of each light
	static bool		rooted;
	int				i;

	if (!r_affinetridesc.rgblight)
		return light;
	if (!rooted)
	{
		for (i = 0 ; i <= (255 << 6) ; i++)
			root[i] = (short)((255 << 6) - (int)(sqrtf (sqrtf ((float)((255 << 6) - i) / 8192.0f)) * 8192.0f + 0.5f));
		rooted = true;
	}
	return root[light < (255 << 6) ? light : 255 << 6];
}

/*
================
R_AliasFlashVert

A vertex of a view model's muzzle flash, which is hidden behind the view: it
crosses the view's plane, and far. Both are ezQuake's tests, of now and of
old; parts of the guns cross the plane too, by 5 units at most, and a flash
goes 12 to 84 (id's view models).
================
*/
#define	FLASH_MINMOVE	6.3f		// units (ezQuake's)

static bool R_AliasFlashVert (const trivertx_t *from, const trivertx_t *to)
{
	float	d[3];
	int		j;

	if ((from->v[0] * pmdl->scale[0] + pmdl->scale_origin[0] > 0)
		== (to->v[0] * pmdl->scale[0] + pmdl->scale_origin[0] > 0))
		return false;
	for (j=0 ; j<3 ; j++)
		d[j] = (to->v[j] - from->v[j]) * pmdl->scale[j];
	return DotProduct (d, d) > FLASH_MINMOVE * FLASH_MINMOVE;
}

/*
================
R_AliasVertex

Vertex vert on the model's grid, and the cosine of its normal and the light:
between the pose turned from and the frame's when turning (r_lerpframes). A
view model's muzzle flash is at the frame's at once, not drawn out from
behind the view (r_lerpmuzzlehack).
================
*/
static float R_AliasVertex (int vert, vec3_t v)
{
	const trivertx_t	*to = &r_apverts[vert], *from;
	float				f, lightfrom, lightto;

	lightto = DotProduct (r_avertexnormals[to->lightnormalindex], r_plightvec);
	if (!r_aoldverts)
	{
		v[0] = (float)to->v[0];
		v[1] = (float)to->v[1];
		v[2] = (float)to->v[2];
		return lightto;
	}

	from = &r_aoldverts[vert];
	f = r_alerp;
	if (r_amuzzlehack && R_AliasFlashVert (from, to))
		f = 1;
	v[0] = from->v[0] + (to->v[0] - from->v[0]) * f;
	v[1] = from->v[1] + (to->v[1] - from->v[1]) * f;
	v[2] = from->v[2] + (to->v[2] - from->v[2]) * f;
	lightfrom = DotProduct (r_avertexnormals[from->lightnormalindex], r_plightvec);
	return lightfrom + (lightto - lightfrom) * f;
}

/*
================
R_AliasTransformFinalVert
================
*/
static void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	int vert, stvert_t *pstverts)
{
	int		temp;
	float	lightcos;
	vec3_t	v;

	lightcos = R_AliasVertex (vert, v);
	av->fv[0] = DotProduct(v, aliastransform[0]) +
			aliastransform[0][3];
	av->fv[1] = DotProduct(v, aliastransform[1]) +
			aliastransform[1][3];
	av->fv[2] = DotProduct(v, aliastransform[2]) +
			aliastransform[2][3];

	fv->v[2] = pstverts->s;
	fv->v[3] = pstverts->t;

	fv->flags = pstverts->onseam;

// lighting
	temp = r_ambientlight;

	if (lightcos < 0)
	{
		temp += (int)(r_shadelight * lightcos);

	// clamp; because we limited the minimum ambient and shading light, we
	// don't have to clamp low light, just bright
		if (temp < 0)
			temp = 0;
	}

	fv->v[4] = R_AliasVertexLight (temp);
}



/*
================
R_AliasTransformAndProjectFinalVerts
================
*/
static void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv, stvert_t *pstverts)
{
	int			i, temp;
	float		lightcos, zi;
	vec3_t		v;

	for (i=0 ; i<r_anumverts ; i++, fv++, pstverts++)
	{
		lightcos = R_AliasVertex (i, v);

	// transform and project
		zi = 1.0f / (DotProduct(v, aliastransform[2]) +
				aliastransform[2][3]);

	// x, y, and z are scaled down by 1/2**31 in the transform, so 1/z is
	// scaled up by 1/2**31, and the scaling cancels out for x and y in the
	// projection
		fv->v[5] = R_AliasFixedZi (zi * r_aliaszmul);

		fv->v[0] = (int)(((DotProduct(v, aliastransform[0]) +
				aliastransform[0][3]) * zi) + aliasxcenter);
		fv->v[1] = (int)(((DotProduct(v, aliastransform[1]) +
				aliastransform[1][3]) * zi) + aliasycenter);

		fv->v[2] = pstverts->s;
		fv->v[3] = pstverts->t;
		fv->flags = pstverts->onseam;

	// lighting
		temp = r_ambientlight;

		if (lightcos < 0)
		{
			temp += (int)(r_shadelight * lightcos);

		// clamp; because we limited the minimum ambient and shading light, we
		// don't have to clamp low light, just bright
			if (temp < 0)
				temp = 0;
		}

		fv->v[4] = R_AliasVertexLight (temp);
	}
}



/*
================
R_AliasProjectFinalVert
================
*/
void R_AliasProjectFinalVert (finalvert_t *fv, auxvert_t *av)
{
	float	zi;

// project points
	zi = 1.0f / av->fv[2];

	fv->v[5] = R_AliasFixedZi (zi * ziscale * r_aliaszmul);

	fv->v[0] = (int)((av->fv[0] * aliasxscale * zi) + aliasxcenter);
	fv->v[1] = (int)((av->fv[1] * aliasyscale * zi) + aliasycenter);
}


/*
================
R_AliasPrepareUnclippedPoints
================
*/
static void R_AliasPrepareUnclippedPoints (void)
{
	stvert_t	*pstverts;
	finalvert_t	*fv;

	pstverts = (stvert_t *)((byte *)paliashdr + paliashdr->stverts);
	r_anumverts = pmdl->numverts;
// FIXME: just use pfinalverts directly?
	fv = pfinalverts;

	R_AliasTransformAndProjectFinalVerts (fv, pstverts);

	if (r_affinetridesc.drawtype)
		D_PolysetDrawFinalVerts (fv, r_anumverts);

	r_affinetridesc.pfinalverts = pfinalverts;
	r_affinetridesc.ptriangles = (mtriangle_t *)
			((byte *)paliashdr + paliashdr->triangles);
	r_affinetridesc.numtriangles = pmdl->numtris;

	D_PolysetDraw ();
}

/*
===============
R_AliasSetupSkin
===============
*/
static void R_AliasSetupSkin (void)
{
	int					skinnum;
	int					i, numskins;
	maliasskingroup_t	*paliasskingroup;
	float				*pskinintervals, fullskininterval;
	float				skintargettime, skintime;

	skinnum = currententity->skinnum;
	if ((skinnum >= pmdl->numskins) || (skinnum < 0))
	{
		Con_DPrintf ("R_AliasSetupSkin: no such skin # %d\n", skinnum);
		skinnum = 0;
	}

	pskindesc = ((maliasskindesc_t *)
			((byte *)paliashdr + paliashdr->skindesc)) + skinnum;
	a_skinwidth = pmdl->skinwidth;

	if (pskindesc->type == ALIAS_SKIN_GROUP)
	{
		paliasskingroup = (maliasskingroup_t *)((byte *)paliashdr +
				pskindesc->skin);
		pskinintervals = (float *)
				((byte *)paliashdr + paliasskingroup->intervals);
		numskins = paliasskingroup->numskins;
		fullskininterval = pskinintervals[numskins-1];
	
		skintime = (float)(r_scene.time + currententity->syncbase);
	
	// when loading in Mod_LoadAliasSkinGroup, we guaranteed all interval
	// values are positive, so we don't have to worry about division by 0
		skintargettime = skintime -
				((int)(skintime / fullskininterval)) * fullskininterval;
	
		for (i=0 ; i<(numskins-1) ; i++)
		{
			if (pskinintervals[i] > skintargettime)
				break;
		}
	
		pskindesc = &paliasskingroup->skindescs[i];
	}

	r_affinetridesc.pskindesc = pskindesc;
	r_affinetridesc.pskin = (void *)((byte *)paliashdr + pskindesc->skin);
	r_affinetridesc.skinwidth = a_skinwidth;
	r_affinetridesc.seamfixupX16 = a_skinwidth << 15;	// half the skin, an odd one too
	r_affinetridesc.skinheight = pmdl->skinheight;
	r_affinetridesc.holey = (currententity->model->flags & MF_HOLEY) != 0;

	if (currententity->skin)
	{
		r_affinetridesc.pskin = currententity->skin;
		r_affinetridesc.skinwidth = 320;
		r_affinetridesc.skinheight = 200;
	}
}

/*
================
R_AliasSetupLighting
================
*/
static void R_AliasSetupLighting (alight_t *plighting)
{
	int		i;

// guarantee that no vertex will ever be lit below LIGHT_MIN, so we don't have
// to clamp off the bottom
	r_ambientlight = plighting->ambientlight;

	if (r_ambientlight < LIGHT_MIN)
		r_ambientlight = LIGHT_MIN;

	r_ambientlight = (255 - r_ambientlight) << VID_CBITS;

	if (r_ambientlight < LIGHT_MIN)
		r_ambientlight = LIGHT_MIN;

	r_shadelight = (float)plighting->shadelight;

	if (r_shadelight < 0)
		r_shadelight = 0;

	r_shadelight *= VID_GRADES;

	r_affinetridesc.rgblight = r_lightmode.value != 0;
	for (i=0 ; i<3 ; i++)		// the tint multiplies light, so as its fourth root
		r_affinetridesc.tint[i] = (unsigned)(sqrtf (sqrtf (plighting->color[i])) * 256 + 0.5f);

// rotate the lighting vector into the model's frame of reference
	r_plightvec[0] = DotProduct (plighting->plightvec, alias_forward);
	r_plightvec[1] = -DotProduct (plighting->plightvec, alias_right);
	r_plightvec[2] = DotProduct (plighting->plightvec, alias_up);
}

/*
=================
R_AliasPose

A frame's pose: a group's by the time
=================
*/
static trivertx_t *R_AliasPose (int frame)
{
	int				i, numframes;
	maliasgroup_t	*paliasgroup;
	float			*pintervals, fullinterval, targettime, time;

	if (paliashdr->frames[frame].type == ALIAS_SINGLE)
		return (trivertx_t *)((byte *)paliashdr + paliashdr->frames[frame].frame);
	
	paliasgroup = (maliasgroup_t *)
				((byte *)paliashdr + paliashdr->frames[frame].frame);
	pintervals = (float *)((byte *)paliashdr + paliasgroup->intervals);
	numframes = paliasgroup->numframes;
	fullinterval = pintervals[numframes-1];

	time = (float)(r_scene.time + currententity->syncbase);

//
// when loading in Mod_LoadAliasGroup, we guaranteed all interval values
// are positive, so we don't have to worry about division by 0
//
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i=0 ; i<(numframes-1) ; i++)
	{
		if (pintervals[i] > targettime)
			break;
	}

	return (trivertx_t *)((byte *)paliashdr + paliasgroup->frames[i].frame);
}

/*
=================
R_AliasSetupFrame

The frame's pose, and the pose turned from when turning (r_lerpframes)
=================
*/
static void R_AliasSetupFrame (void)
{
	int		frame;

	frame = currententity->frame;
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d\n", frame);
		frame = 0;
	}
	r_apverts = R_AliasPose (frame);

	r_aoldverts = NULL;
	if (R_AliasTurning (paliashdr))
	{
		r_aoldverts = R_AliasPose (currententity->oldframe);
		if (r_aoldverts == r_apverts)
			r_aoldverts = NULL;
		r_alerp = currententity->backlerp < 1 ? 1 - currententity->backlerp : 0;
	}

	// the view models with a muzzle flash, the axe hasn't (ezQuake's)
	r_amuzzlehack = r_aoldverts && r_lerpmuzzlehack.value && currententity == r_scene.viewent
		&& !strncmp (currententity->model->name, "progs/v_", 8)
		&& strcmp (currententity->model->name, "progs/v_axe.mdl");
}


/*
================
R_AliasDrawModel
================
*/
void R_AliasDrawModel (alight_t *plighting)
{
	finalvert_t		finalverts[MAXALIASVERTS +
						((CACHE_SIZE - 1) / sizeof(finalvert_t)) + 1];
	auxvert_t		auxverts[MAXALIASVERTS];

	r_amodels_drawn++;

// cache align
	pfinalverts = (finalvert_t *)
			(((uintptr_t)&finalverts[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	pauxverts = &auxverts[0];

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);
	pmdl = (mdl_t *)((byte *)paliashdr + paliashdr->model);

	R_AliasSetupSkin ();
	R_AliasSetUpTransform (currententity->trivial_accept);
	R_AliasSetupLighting (plighting);
	R_AliasSetupFrame ();

	r_affinetridesc.drawtype = (currententity->trivial_accept == 3) &&
			r_recursiveaffinetriangles;

	// a player's colors: through the colormap a remap of the skin's colors,
	// in RGB the palette in them, which has colors the palette hasn't (skin.c)
	if (r_affinetridesc.rgblight && currententity->palette)
	{
		r_affinetridesc.skinremap = r_identityremap;
		r_affinetridesc.palette = currententity->palette;
	}
	else
	{
		r_affinetridesc.skinremap = currententity->translate ? currententity->translate : r_identityremap;
		r_affinetridesc.palette = d_pal30;
	}

	if (currententity != r_scene.viewent)
		r_aliaszmul = 1;
	else
		r_aliaszmul = 3;
	ziscale = (float)0x8000 * (float)0x10000;

	if (currententity->trivial_accept)
		R_AliasPrepareUnclippedPoints ();
	else
		R_AliasPreparePoints ();
}

