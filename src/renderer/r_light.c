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
// r_light.c

#include "r_local.h"
#include "r_local.h"

int	r_dlightframecount;


/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			i,j,k;
	
//
// light animations
// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = (int)(r_scene.time*10);
	for (j=0 ; j<MAX_LIGHTSTYLES ; j++)
	{
		if (!r_scene.lightstyles[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		k = i % r_scene.lightstyles[j].length;
		k = r_scene.lightstyles[j].map[k] - 'a';
		k = k*22;
		d_lightstylevalue[j] = k;
	}	
}


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights
=============
*/
void R_MarkLights (dlight_t *light, int bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;
	
	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;
	
	if (dist > light->radius)
	{
		R_MarkLights (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		R_MarkLights (light, bit, node->children[1]);
		return;
	}
		
// mark the polygons
	surf = r_scene.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<(int)node->numsurfaces ; i++, surf++)
	{
		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}
		surf->dlightbits |= bit;
	}

	R_MarkLights (light, bit, node->children[0]);
	R_MarkLights (light, bit, node->children[1]);
}


/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int		i;
	dlight_t	*l;

	r_dlightframecount = r_framecount + 1;	// because the count hasn't
											//  advanced yet for this frame
	l = r_scene.dlights;

	for (i=0 ; i<MAX_DLIGHTS ; i++, l++)
	{
		if (l->die < r_scene.time || !l->radius)
			continue;
		R_MarkLights ( l, 1<<i, r_scene.worldmodel->nodes );
	}
}


/*
=============
RecursiveLightPoint

The light where the line from start to end first hits the world, from the
lightmap there: mono, and in color; -1 if it hits nothing
=============
*/
static int RecursiveLightPoint (mnode_t *node, vec3_t start, vec3_t end, vec3_t color)
{
	int			r;
	float		front, back, frac, fs, ft;
	int			side;
	mplane_t	*plane;
	vec3_t		mid;
	msurface_t	*surf;
	int			s, t, ds, dt, u, v;
	int			i, c, maps, index, planesize;
	mtexinfo_t	*tex;
	unsigned	scale, rgb[3];

	if (node->contents < 0)
		return -1;		// didn't hit anything

// calculate mid point

// FIXME: optimize for axial
	plane = node->plane;
	front = DotProduct (start, plane->normal) - plane->dist;
	back = DotProduct (end, plane->normal) - plane->dist;
	side = front < 0;

	if ( (back < 0) == side)
		return RecursiveLightPoint (node->children[side], start, end, color);

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;

// go down front side
	r = RecursiveLightPoint (node->children[side], start, mid, color);
	if (r >= 0)
		return r;		// hit something

	if ( (back < 0) == side )
		return -1;		// didn't hit anuthing

// check for impact on this node

	surf = r_scene.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<(int)node->numsurfaces ; i++, surf++)
	{
		if (surf->flags & SURF_DRAWTILED)
			continue;	// no lightmaps

		tex = surf->texinfo;

		fs = DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3];
		ft = DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3];
		s = (int)fs;
		t = (int)ft;

		if (s < surf->texturemins[0] ||
		t < surf->texturemins[1])
			continue;

		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];

		if ( ds > surf->extents[0] || dt > surf->extents[1] )
			continue;

		color[0] = color[1] = color[2] = 0;
		if (!surf->samples)
			return 0;

	// the luxel the point falls on
		if (surf->lmvanilla)
		{
			u = ds >> 4;
			v = dt >> 4;
		}
		else
		{
			u = (int)(surf->lmvecs[0][0]*fs + surf->lmvecs[0][1]*ft + surf->lmvecs[0][2]);
			v = (int)(surf->lmvecs[1][0]*fs + surf->lmvecs[1][1]*ft + surf->lmvecs[1][2]);
			u = u < 0 ? 0 : (u >= surf->lmwidth ? surf->lmwidth - 1 : u);
			v = v < 0 ? 0 : (v >= surf->lmheight ? surf->lmheight - 1 : v);
		}
		index = v * surf->lmwidth + u;
		planesize = surf->lmwidth * surf->lmheight;

		r = 0;
		rgb[0] = rgb[1] = rgb[2] = 0;
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ; maps++)
		{
			scale = d_lightstylevalue[surf->styles[maps]];
			r += surf->samples[index] * scale;
			if (surf->samples_rgb)
				for (c=0 ; c<3 ; c++)
					rgb[c] += surf->samples_rgb[index*3 + c] * scale;
			index += planesize;
		}
		r >>= 8;
		for (c=0 ; c<3 ; c++)
			color[c] = surf->samples_rgb ? (float)(rgb[c] >> 12) : (float)r;	// 2048 is 128
		return r;
	}

// go down back side
	return RecursiveLightPoint (node->children[!side], mid, end, color);
}

/*
=============
R_LightPoint

The light of the world under p: mono, and in color with the same scale
=============
*/
int R_LightPoint (vec3_t p, vec3_t color)
{
	vec3_t		end;
	int			r, c;

	if (!r_scene.worldmodel->lightdata)
	{
		color[0] = color[1] = color[2] = 255;
		return 255;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;

	r = RecursiveLightPoint (r_scene.worldmodel->nodes, p, end, color);

	if (r == -1)
	{
		r = 0;
		color[0] = color[1] = color[2] = 0;
	}

	if (r < r_refdef.ambientlight)
		r = r_refdef.ambientlight;
	for (c=0 ; c<3 ; c++)
		if (color[c] < r_refdef.ambientlight)
			color[c] = (float)r_refdef.ambientlight;

	return r;
}

